// FST-X core engine: errors, hashing, file I/O and the worker pool (the codecs are in codec_ext.cpp). No R headers.
// 64-bit off_t for fseeko / ftruncate on 32-bit POSIX systems too (before any system header; no effect on 64-bit).
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "core_internal.h"

#include <algorithm>
#include <cctype>
#include <new>
#include <cstdlib>
#include <string>
#include <map>

#define XXH_INLINE_ALL
#include "vendor/xxhash/xxhash.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fstx {

// ---------------------------------------------------------------- errors
void fail(int code, const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  throw Error(code, buf);
}

void set_error(fstx_error* err, int code, const char* msg, int64_t column) {
  if (!err) return;
  err->code = code;
  err->column = column;
  std::snprintf(err->message, sizeof(err->message), "%s", msg ? msg : "");
}

// ---------------------------------------------------------------- hashing
uint64_t hash64(const void* p, size_t n) { return (uint64_t)XXH3_64bits(p, n); }

// ---------------------------------------------------------------- UTF-8 (column names and time zones)
bool valid_utf8(const char* str, size_t n) {
  const unsigned char* s = (const unsigned char*)str;
  size_t i = 0;
  while (i < n) {
    const unsigned c = s[i];
    if (c < 0x80) {
      ++i;
      continue;
    }
    int k;
    unsigned lo = 0x80, hi = 0xBF;  // allowed range of the second byte
    if (c >= 0xC2 && c <= 0xDF) k = 1;
    else if (c >= 0xE0 && c <= 0xEF) k = 2, lo = c == 0xE0 ? 0xA0 : 0x80, hi = c == 0xED ? 0x9F : 0xBF;
    else if (c >= 0xF0 && c <= 0xF4) k = 3, lo = c == 0xF0 ? 0x90 : 0x80, hi = c == 0xF4 ? 0x8F : 0xBF;
    else return false;
    if (n - i <= (size_t)k || s[i + 1] < lo || s[i + 1] > hi) return false;
    for (int j = 2; j <= k; ++j)
      if ((s[i + j] & 0xC0) != 0x80) return false;
    i += (size_t)k + 1;
  }
  return true;
}

// ---------------------------------------------------------------- files
#ifdef _WIN32
static std::wstring widen(const std::string& s) {
  if (s.empty()) fail(FSTX_ERR_ARG, "empty file path");
  if (s.size() > 0x7fffffff) fail(FSTX_ERR_ARG, "file path too long");
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
  if (n <= 0) fail(FSTX_ERR_ARG, "file path is not valid UTF-8");
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), &w[0], n);
  return w;
}

static std::string win_error_text(DWORD e) {
  const char* what = "";
  switch (e) {
    case ERROR_FILE_NOT_FOUND: what = "file not found"; break;
    case ERROR_PATH_NOT_FOUND: what = "folder not found"; break;
    case ERROR_ACCESS_DENIED: what = "access denied"; break;
    case ERROR_SHARING_VIOLATION: what = "file is in use by another process"; break;
    case ERROR_USER_MAPPED_FILE: what = "file is open (memory-mapped) elsewhere"; break;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: what = "disk full"; break;
    default: what = "system error"; break;
  }
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s, Windows error %lu", what, (unsigned long)e);
  return buf;
}

// FILE_ID_INFO and FileIdInfo (18) of Windows 8+, spelled out here: the headers declare them only for
// _WIN32_WINNT >= 0x0602, and Rtools45 g++ defaults to 0x0601 (_mingw.h; R passes no -D_WIN32_WINNT).
struct FstxFileIdInfo {
  ULONGLONG volume;
  BYTE id[16];
};
static_assert(sizeof(FstxFileIdInfo) == 24, "FstxFileIdInfo must match the 24-byte FILE_ID_INFO");

static FileId id_of_handle(HANDLE h) {
  FileId id;
  FstxFileIdInfo xi;
  BY_HANDLE_FILE_INFORMATION fi;
  if (GetFileInformationByHandleEx(h, (FILE_INFO_BY_HANDLE_CLASS)18, &xi, sizeof(xi))) {
    // Only the low 32 bits of the volume serial: that is what the fallback below gets (dwVolumeSerialNumber), so an
    // NTFS or FAT file has the same id whichever call answered (file_id_of_path and MappedFile::open open the file
    // with different access). The 128-bit file id alone still tells ReFS files apart.
    id.a = (uint32_t)xi.volume;
    std::memcpy(&id.c, xi.id, 8);
    std::memcpy(&id.b, xi.id + 8, 8);
  } else if (GetFileInformationByHandle(h, &fi)) {  // older Windows or a file system without FileIdInfo
    id.a = (uint64_t)fi.dwVolumeSerialNumber;
    id.c = ((uint64_t)fi.nFileIndexHigh << 32) | (uint64_t)fi.nFileIndexLow;
  } else {
    return id;
  }
  // Some FAT volumes and network redirectors give index 0 for every file: no usable id, so the file is never taken
  // for another open one.
  id.ok = id.b != 0 || id.c != 0;
  return id;
}

FileId file_id_of_path(const std::string& path) {
  std::wstring w = widen(path);
  HANDLE h = CreateFileW(w.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return FileId();
  FileId id = id_of_handle(h);
  CloseHandle(h);
  return id;
}

MappedFile::~MappedFile() {
  if (data_) UnmapViewOfFile(data_);
  if (mapping_) CloseHandle((HANDLE)mapping_);
  if (rfile_) CloseHandle((HANDLE)rfile_);
  for (void* e : ev_free_) CloseHandle((HANDLE)e);
  if (file_ && file_ != INVALID_HANDLE_VALUE) CloseHandle((HANDLE)file_);
}

void MappedFile::open(const std::string& path) {
  std::wstring w = widen(path);
  HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) fail(FSTX_ERR_IO, "cannot open '%s' (%s)", path.c_str(), win_error_text(GetLastError()).c_str());
  file_ = h;
  id_ = id_of_handle(h);
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(h, &sz)) fail(FSTX_ERR_IO, "cannot get the size of '%s'", path.c_str());
  size_ = (uint64_t)sz.QuadPart;
  if (size_ == 0) return;  // nothing to map; the caller reports the file as too small
  HANDLE m = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!m) fail(FSTX_ERR_IO, "cannot map '%s' (%s)", path.c_str(), win_error_text(GetLastError()).c_str());
  mapping_ = m;
  void* v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
  if (!v) fail(FSTX_ERR_IO, "cannot map '%s' (%s)", path.c_str(), win_error_text(GetLastError()).c_str());
  data_ = (const uint8_t*)v;
  // ReOpenFile: the same file (never another one at the same path), opened for overlapped reads so that threads
  // can read at their own offsets at once; read_at copies from the mapping if this is not possible
  HANDLE r = ReOpenFile(h, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_FLAG_OVERLAPPED);
  rfile_ = r == INVALID_HANDLE_VALUE ? nullptr : r;
}

void MappedFile::release(uint64_t off, uint64_t n) const {
  if (!data_ || n == 0 || off >= size_) return;
  const uint64_t a = off & ~(uint64_t)4095, e = std::min(size_, off + n);
  VirtualUnlock((LPVOID)(data_ + a), (SIZE_T)(e - a));  // not locked: this just trims the working set
}

void MappedFile::read_at(uint64_t off, void* dst, size_t n) const {
  if (off > size_ || n > size_ - off) fail(FSTX_ERR_INTERNAL, "read outside the file");
  uint8_t* d = (uint8_t*)dst;
  HANDLE ev = nullptr;  // an event of this file's pool (taken here, given back below)
  if (n && rfile_) {
    std::lock_guard<std::mutex> g(ev_m_);
    if (!ev_free_.empty()) {
      ev = (HANDLE)ev_free_.back();
      ev_free_.pop_back();
    }
  }
  if (n && rfile_ && !ev) ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  struct GiveBack {
    const MappedFile& f;
    HANDLE e;
    ~GiveBack() {
      if (!e) return;
      std::lock_guard<std::mutex> g(f.ev_m_);
      try {
        f.ev_free_.push_back(e);
      } catch (...) {
        CloseHandle(e);
      }
    }
  } give_back{*this, ev};
  while (n && rfile_ && ev) {
    OVERLAPPED o;
    std::memset(&o, 0, sizeof(o));
    o.Offset = (DWORD)(off & 0xFFFFFFFFull);
    o.OffsetHigh = (DWORD)(off >> 32);
    o.hEvent = ev;
    const DWORD want = (DWORD)std::min<size_t>(n, (size_t)1 << 30);
    DWORD got = 0;
    if (!ReadFile((HANDLE)rfile_, d, want, &got, &o)) {
      if (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult((HANDLE)rfile_, &o, &got, TRUE)) break;
    }
    if (got == 0) break;
    d += got;
    off += got;
    n -= got;
  }
  if (n) std::memcpy(d, data_ + off, n);  // no reopened handle, or the read stopped: the mapping has the bytes
}

OutFile::~OutFile() { close_and_delete(); }
bool OutFile::is_open() const { return file_ != nullptr; }

static void refuse_if_open(const std::string& path) {
  if (registry_has(file_id_of_path(path)))
    fail(FSTX_ERR_IO,
         "cannot write '%s': it is open in this R session, memory-mapped by a lazy read_fstx() result that is "
         "still alive. Write to another path, or remove those objects (rm()) first",
         path.c_str());
}

void OutFile::create(const std::string& path) {
  path_ = path;
  refuse_if_open(path);
  std::wstring w = widen(path);
  HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) fail(FSTX_ERR_IO, "cannot create '%s' (%s)", path.c_str(), win_error_text(GetLastError()).c_str());
  file_ = h;
  pos_ = 0;
  buf_.reserve(1 << 20);
}

static void win_write_all(HANDLE h, const uint8_t* p, size_t n, const std::string& path) {
  while (n > 0) {
    DWORD chunk = (DWORD)std::min<size_t>(n, (size_t)1 << 30), done = 0;
    if (!WriteFile(h, p, chunk, &done, nullptr) || done == 0)
      fail(FSTX_ERR_IO, "write to '%s' failed (%s)", path.c_str(), win_error_text(GetLastError()).c_str());
    p += done;
    n -= done;
  }
}

void OutFile::flush_buffer() {
  if (buf_.empty()) return;
  win_write_all((HANDLE)file_, buf_.data(), buf_.size(), path_);
  buf_.clear();
}

void OutFile::write(const void* p, size_t n) {
  if (!file_) fail(FSTX_ERR_INTERNAL, "write to a closed file");
  if (n >= 65536) {
    flush_buffer();
    win_write_all((HANDLE)file_, (const uint8_t*)p, n, path_);
  } else {
    if (buf_.size() + n > (1u << 20)) flush_buffer();
    buf_.insert(buf_.end(), (const uint8_t*)p, (const uint8_t*)p + n);
  }
  pos_ += n;
}

void OutFile::close() {
  if (!file_) return;
  flush_buffer();
  HANDLE h = (HANDLE)file_;
  file_ = nullptr;
  if (!CloseHandle(h)) fail(FSTX_ERR_IO, "closing '%s' failed", path_.c_str());
}

void OutFile::close_and_delete() {
  if (!file_) return;
  CloseHandle((HANDLE)file_);
  file_ = nullptr;
  if (existing_) return;
  try {
    std::wstring w = widen(path_);
    DeleteFileW(w.c_str());
  } catch (...) {
  }
}

void OutFile::open_existing(const std::string& path) {
  path_ = path;
  refuse_if_open(path);
  std::wstring w = widen(path);
  HANDLE h = CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) fail(FSTX_ERR_IO, "cannot open '%s' for writing (%s)", path.c_str(), win_error_text(GetLastError()).c_str());
  file_ = h;
  existing_ = true;
  LARGE_INTEGER sz, z;
  z.QuadPart = 0;
  if (!GetFileSizeEx(h, &sz) || !SetFilePointerEx(h, z, nullptr, FILE_END)) fail(FSTX_ERR_IO, "cannot seek in '%s'", path.c_str());
  pos_ = (uint64_t)sz.QuadPart;
  buf_.reserve(1 << 20);
}

static void win_seek(HANDLE h, uint64_t off, const std::string& path) {
  LARGE_INTEGER li;
  li.QuadPart = (LONGLONG)off;
  if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) fail(FSTX_ERR_IO, "cannot seek in '%s'", path.c_str());
}

void OutFile::write_at(uint64_t off, const void* p, size_t n) {
  if (!file_) fail(FSTX_ERR_INTERNAL, "write to a closed file");
  flush_buffer();
  win_seek((HANDLE)file_, off, path_);
  win_write_all((HANDLE)file_, (const uint8_t*)p, n, path_);
  win_seek((HANDLE)file_, pos_, path_);
}

void OutFile::truncate(uint64_t size) {
  if (!file_) fail(FSTX_ERR_INTERNAL, "truncate of a closed file");
  flush_buffer();
  win_seek((HANDLE)file_, size, path_);
  if (!SetEndOfFile((HANDLE)file_)) fail(FSTX_ERR_IO, "cannot truncate '%s' (%s)", path_.c_str(), win_error_text(GetLastError()).c_str());
  pos_ = size;
}

void OutFile::sync() {
  if (!file_) return;
  flush_buffer();
  if (!FlushFileBuffers((HANDLE)file_)) fail(FSTX_ERR_IO, "cannot flush '%s' to disk", path_.c_str());
}

#else  // POSIX

FileId file_id_of_path(const std::string& path) {
  FileId id;
  struct stat st;
  if (::stat(path.c_str(), &st) == 0) {
    id.a = (uint64_t)st.st_dev;
    id.b = (uint64_t)st.st_ino;
    id.ok = true;
  }
  return id;
}

MappedFile::~MappedFile() {
  if (data_) munmap((void*)data_, size_);
  if (fd_ >= 0) ::close(fd_);
}

void MappedFile::open(const std::string& path) {
  fd_ = ::open(path.c_str(), O_RDONLY);
  if (fd_ < 0) fail(FSTX_ERR_IO, "cannot open '%s'", path.c_str());
  struct stat st;
  if (fstat(fd_, &st) != 0) fail(FSTX_ERR_IO, "cannot stat '%s'", path.c_str());
  id_.a = (uint64_t)st.st_dev;
  id_.b = (uint64_t)st.st_ino;
  id_.ok = true;
  size_ = (uint64_t)st.st_size;
  if (size_ == 0) return;
  if (size_ != (uint64_t)(size_t)size_) fail(FSTX_ERR_IO, "'%s' is too large to map on this 32-bit system", path.c_str());
  void* v = mmap(nullptr, (size_t)size_, PROT_READ, MAP_SHARED, fd_, 0);
  if (v == MAP_FAILED) fail(FSTX_ERR_IO, "cannot map '%s'", path.c_str());
  data_ = (const uint8_t*)v;
}

void MappedFile::read_at(uint64_t off, void* dst, size_t n) const {
  if (off > size_ || n > size_ - off) fail(FSTX_ERR_INTERNAL, "read outside the file");
  uint8_t* d = (uint8_t*)dst;
  while (n && fd_ >= 0) {
    const ssize_t got = ::pread(fd_, d, std::min<size_t>(n, (size_t)1 << 30), (off_t)off);
    if (got <= 0) break;  // EINTR included: the mapping has the bytes
    d += got;
    off += (uint64_t)got;
    n -= (size_t)got;
  }
  if (n) std::memcpy(d, data_ + off, n);
}

// madvise needs page-aligned addresses: 4 KB on x86, but 16 KB on Apple silicon and 4-64 KB on Linux ARM
static uint64_t page_mask() {
  static const uint64_t m = [] {
    const long p = sysconf(_SC_PAGESIZE);
    return (uint64_t)(p > 0 ? p : 4096) - 1;
  }();
  return m;
}

void MappedFile::release(uint64_t off, uint64_t n) const {
  if (!data_ || n == 0 || off >= size_) return;
  const uint64_t pm = page_mask();
  const uint64_t a = off & ~pm;
  const uint64_t e = (std::min(size_, off + n) + pm) & ~pm;
  madvise((void*)(data_ + a), (size_t)(e - a), MADV_DONTNEED);  // read-only file map: refaults from the cache
}

OutFile::~OutFile() { close_and_delete(); }
bool OutFile::is_open() const { return file_ != nullptr; }

void OutFile::create(const std::string& path) {
  path_ = path;
  if (registry_has(file_id_of_path(path)))
    fail(FSTX_ERR_IO,
         "cannot write '%s': it is open in this R session, memory-mapped by a lazy read_fstx() result that is "
         "still alive. Write to another path, or remove those objects (rm()) first",
         path.c_str());
  file_ = std::fopen(path.c_str(), "wb");
  if (!file_) fail(FSTX_ERR_IO, "cannot create '%s'", path.c_str());
  pos_ = 0;
}
void OutFile::flush_buffer() {}
void OutFile::write(const void* p, size_t n) {
  if (!file_) fail(FSTX_ERR_INTERNAL, "write to a closed file");
  if (n && std::fwrite(p, 1, n, file_) != n) fail(FSTX_ERR_IO, "write to '%s' failed", path_.c_str());
  pos_ += n;
}
void OutFile::close() {
  if (!file_) return;
  FILE* f = file_;
  file_ = nullptr;
  if (std::fclose(f) != 0) fail(FSTX_ERR_IO, "closing '%s' failed", path_.c_str());
}
void OutFile::close_and_delete() {
  if (!file_) return;
  std::fclose(file_);
  file_ = nullptr;
  if (!existing_) std::remove(path_.c_str());
}
void OutFile::open_existing(const std::string& path) {
  path_ = path;
  if (registry_has(file_id_of_path(path)))
    fail(FSTX_ERR_IO,
         "cannot write '%s': it is open in this R session, memory-mapped by a lazy read_fstx() result that is "
         "still alive. Write to another path, or remove those objects (rm()) first",
         path.c_str());
  file_ = std::fopen(path.c_str(), "r+b");
  if (!file_) fail(FSTX_ERR_IO, "cannot open '%s' for writing", path.c_str());
  existing_ = true;
  if (fseeko(file_, 0, SEEK_END) != 0) fail(FSTX_ERR_IO, "cannot seek in '%s'", path.c_str());
  pos_ = (uint64_t)ftello(file_);
}
void OutFile::write_at(uint64_t off, const void* p, size_t n) {
  if (!file_) fail(FSTX_ERR_INTERNAL, "write to a closed file");
  if (fseeko(file_, (off_t)off, SEEK_SET) != 0 || (n && std::fwrite(p, 1, n, file_) != n) ||
      fseeko(file_, (off_t)pos_, SEEK_SET) != 0)
    fail(FSTX_ERR_IO, "write to '%s' failed", path_.c_str());
}
void OutFile::truncate(uint64_t size) {
  if (!file_) fail(FSTX_ERR_INTERNAL, "truncate of a closed file");
  if (std::fflush(file_) != 0 || ftruncate(fileno(file_), (off_t)size) != 0 || fseeko(file_, (off_t)size, SEEK_SET) != 0)
    fail(FSTX_ERR_IO, "cannot truncate '%s'", path_.c_str());
  pos_ = size;
}
void OutFile::sync() {
  if (!file_) return;
  if (std::fflush(file_) != 0) fail(FSTX_ERR_IO, "cannot flush '%s' to disk", path_.c_str());
#ifdef F_FULLFSYNC
  // macOS: fsync() leaves the data in the drive's cache; F_FULLFSYNC flushes it (fsync if the file system refuses)
  if (fcntl(fileno(file_), F_FULLFSYNC) == 0) return;
#endif
  if (fsync(fileno(file_)) != 0) fail(FSTX_ERR_IO, "cannot flush '%s' to disk", path_.c_str());
}
#endif

void OutFile::pad_to(uint64_t pos) {
  static const uint8_t zeros[kAlign] = {0};
  if (pos < pos_) fail(FSTX_ERR_INTERNAL, "pad_to moves backwards");
  while (pos_ < pos) write(zeros, (size_t)std::min<uint64_t>(pos - pos_, kAlign));
}

// ---------------------------------------------------------------- open-file registry
static std::mutex& registry_mutex() {
  static std::mutex* m = new std::mutex();
  return *m;
}
static std::map<FileId, int>& registry() {
  static std::map<FileId, int>* r = new std::map<FileId, int>();
  return *r;
}
void registry_add(const FileId& id) {
  if (!id.ok) return;
  std::lock_guard<std::mutex> g(registry_mutex());
  ++registry()[id];
}
void registry_remove(const FileId& id) {
  if (!id.ok) return;
  std::lock_guard<std::mutex> g(registry_mutex());
  auto it = registry().find(id);
  if (it != registry().end() && --it->second <= 0) registry().erase(it);
}
bool registry_has(const FileId& id) {
  if (!id.ok) return false;
  std::lock_guard<std::mutex> g(registry_mutex());
  return registry().count(id) > 0;
}

// ---------------------------------------------------------------- thread pool
ThreadPool& ThreadPool::instance() {
  // Never destroyed: joining threads during process exit can deadlock on Windows.
  // fstx_shutdown() stops the workers when the R package is unloaded.
  static ThreadPool* p = [] {
    ThreadPool* q = new ThreadPool();
#ifndef _WIN32
    // the child of fork() (R's parallel::mclapply) has none of the parent's workers: it starts its own
    pthread_atfork(nullptr, nullptr, [] { ThreadPool::instance().after_fork(); });
#endif
    return q;
  }();
  return *p;
}

// In the child of fork(), only the forking thread exists: the parent's worker and spawner threads are gone, and
// the mutex and condition variables may still count their waiters, so they are made again (the old ones and the
// thread handles are left as they are: never destroyed, joined or detached).
void ThreadPool::after_fork() {
  new (&m_) std::mutex();
  new (&cv_work_) std::condition_variable();
  new (&cv_done_) std::condition_variable();
  new (&cv_spawn_) std::condition_variable();
  std::vector<std::thread>* gone = new std::vector<std::thread>(std::move(threads_));
  gone->push_back(std::move(spawner_));
  threads_ = std::vector<std::thread>();
  spawner_ = std::thread();
  job_ = nullptr;
  error_ = nullptr;
  generation_ = 0;
  active_ = next_slot_ = remaining_ = want_ = 0;
  stop_ = running_ = false;
}

ThreadPool::~ThreadPool() { shutdown(); }

void ThreadPool::run_slots(std::unique_lock<std::mutex>& lk) {
  while (running_ && next_slot_ < active_) {
    const int s = next_slot_++;
    lk.unlock();  // job_ stays set: this slot is not finished, so wait() cannot return and clear it
    try {
      job_(s);
    } catch (...) {
      std::lock_guard<std::mutex> g(m_);
      if (!error_) error_ = std::current_exception();
    }
    lk.lock();
    if (--remaining_ == 0) cv_done_.notify_all();
  }
}

void ThreadPool::worker_main() {
  std::unique_lock<std::mutex> lk(m_);
  uint64_t seen = running_ ? generation_ - 1 : generation_;  // a job already running: help with it
  for (;;) {
    cv_work_.wait(lk, [&] { return stop_ || generation_ != seen; });
    if (stop_) return;
    seen = generation_;
    run_slots(lk);
  }
}

// The spawner makes threads one after another until there are want_ (m_ is not held while one is made), then
// waits for the next request. It stays up like the workers until shutdown() joins it: a spawner that had finished
// but was not joined yet was a "thread leak" at process exit under TSan, and a detached one could still be running
// its last instructions when the library is unloaded.
void ThreadPool::spawn_loop() {
  std::unique_lock<std::mutex> lk(m_);
  for (;;) {
    cv_spawn_.wait(lk, [&] { return stop_ || (int)threads_.size() < want_; });
    if (stop_) return;
    lk.unlock();
    std::thread t;
    try {
      t = std::thread([this] { worker_main(); });
    } catch (...) {  // no thread: the pool works with the ones it has (the caller runs any slot left)
      lk.lock();
      want_ = (int)threads_.size();
      continue;
    }
    lk.lock();
    threads_.push_back(std::move(t));
  }
}

void ThreadPool::grow(int k) {
  if (stop_) return;
  if (k > want_) want_ = std::min(k, 256);
  if ((int)threads_.size() >= want_) return;
  if (!spawner_.joinable()) {
    try {
      spawner_ = std::thread([this] { spawn_loop(); });
    } catch (...) {
      return;  // no spawner: the caller runs the slots
    }
  }
  cv_spawn_.notify_one();
}

void ThreadPool::warm(int k) {
  std::lock_guard<std::mutex> g(m_);
  grow(k);
}

void ThreadPool::start(int k, std::function<void(int)> job) {
  std::unique_lock<std::mutex> lk(m_);
  if (running_) fail(FSTX_ERR_INTERNAL, "worker pool is already running a job");
  if (k < 1) k = 1;
  job_ = std::move(job);
  active_ = k;
  next_slot_ = 0;
  remaining_ = k;
  error_ = nullptr;
  running_ = true;
  ++generation_;
  grow(k);
  cv_work_.notify_all();
}

bool ThreadPool::busy() {
  std::lock_guard<std::mutex> g(m_);
  return running_;
}

void ThreadPool::wait() {
  std::unique_lock<std::mutex> lk(m_);
  if (!running_) return;
  run_slots(lk);  // slots no worker has claimed yet run here (workers still starting up are not waited for)
  cv_done_.wait(lk, [&] { return remaining_ == 0; });
  running_ = false;
  job_ = nullptr;
  std::exception_ptr e = error_;
  error_ = nullptr;
  lk.unlock();
  if (e) std::rethrow_exception(e);
}

void ThreadPool::shutdown() {
  {
    std::lock_guard<std::mutex> g(m_);
    stop_ = true;
  }
  cv_work_.notify_all();
  cv_spawn_.notify_all();
  if (spawner_.joinable()) spawner_.join();  // first: it may still add a thread
  for (auto& t : threads_)
    if (t.joinable()) t.join();
  threads_.clear();
  std::lock_guard<std::mutex> g(m_);
  stop_ = false;
  running_ = false;
  want_ = 0;
}

int resolve_threads(int threads) {
  if (threads <= 0) threads = (int)fstx_hardware_threads();
  return std::min(threads, 256);
}

}  // namespace fstx

// Default thread count: all cores, but at most 2 when R CMD check limits cores (_R_CHECK_LIMIT_CORES_ set and
// not "false", as in package 'parallel') and at most OMP_THREAD_LIMIT when that is set.
extern "C" unsigned fstx_hardware_threads(void) {
  unsigned h = std::thread::hardware_concurrency();
  if (!h) h = 1;
  const char* lim = std::getenv("_R_CHECK_LIMIT_CORES_");
  if (lim && *lim) {
    std::string v(lim);
    for (char& c : v) c = (char)std::tolower((unsigned char)c);
    if (v != "false") h = std::min(h, 2u);
  }
  const char* omp = std::getenv("OMP_THREAD_LIMIT");
  if (omp && *omp) {
    const long n = std::strtol(omp, nullptr, 10);
    if (n >= 1) h = std::min<unsigned>(h, (unsigned)std::min(n, 1024L));
  }
  return h;
}

extern "C" void fstx_shutdown(void) {
  try {
    fstx::ThreadPool::instance().shutdown();
  } catch (...) {
  }
}
