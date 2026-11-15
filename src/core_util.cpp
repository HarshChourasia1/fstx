// FST-X core engine: errors, hashing, file I/O, codecs and the worker pool. No R headers.
#include "core_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <map>

#define XXH_INLINE_ALL
#include "vendor/xxhash/xxhash.h"
#include "vendor/lz4/lz4.h"
#include "vendor/zstd/zstd.h"

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

static FileId id_of_handle(HANDLE h) {
  FileId id;
  BY_HANDLE_FILE_INFORMATION fi;
  if (GetFileInformationByHandle(h, &fi)) {
    id.a = (uint64_t)fi.dwVolumeSerialNumber;
    id.b = ((uint64_t)fi.nFileIndexHigh << 32) | (uint64_t)fi.nFileIndexLow;
    id.ok = true;
  }
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
}

void MappedFile::release(uint64_t off, uint64_t n) const {
  if (!data_ || n == 0 || off >= size_) return;
  const uint64_t a = off & ~(uint64_t)4095, e = std::min(size_, off + n);
  VirtualUnlock((LPVOID)(data_ + a), (SIZE_T)(e - a));  // not locked: this just trims the working set
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
  void* v = mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd_, 0);
  if (v == MAP_FAILED) fail(FSTX_ERR_IO, "cannot map '%s'", path.c_str());
  data_ = (const uint8_t*)v;
}

void MappedFile::release(uint64_t off, uint64_t n) const {
  if (!data_ || n == 0 || off >= size_) return;
  const uint64_t a = off & ~(uint64_t)4095;
  const uint64_t e = (std::min(size_, off + n) + 4095) & ~(uint64_t)4095;
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
  if (std::fflush(file_) != 0 || fsync(fileno(file_)) != 0) fail(FSTX_ERR_IO, "cannot flush '%s' to disk", path_.c_str());
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

// ---------------------------------------------------------------- codecs
CodecContext::~CodecContext() {
  if (zc) ZSTD_freeCCtx((ZSTD_CCtx*)zc);
  if (zd) ZSTD_freeDCtx((ZSTD_DCtx*)zd);
}

int zstd_level_min() { return ZSTD_minCLevel(); }
int zstd_level_max() { return ZSTD_maxCLevel(); }

size_t compress_bound(int codec, size_t n) {
  switch (codec) {
    case FSTX_CODEC_LZ4: return (size_t)LZ4_compressBound((int)n);
    case FSTX_CODEC_ZSTD: return ZSTD_compressBound(n);
    default: return n;
  }
}

size_t compress_page(CodecContext& ctx, int codec, int level, const void* src, size_t n, void* dst, size_t cap) {
  if (codec == FSTX_CODEC_ZSTD) {
    if (!ctx.zc) {
      ctx.zc = ZSTD_createCCtx();
      if (!ctx.zc) fail(FSTX_ERR_MEMORY, "cannot create a ZSTD context");
    }
    size_t r = ZSTD_compressCCtx((ZSTD_CCtx*)ctx.zc, dst, cap, src, n, level);
    if (ZSTD_isError(r)) fail(FSTX_ERR_INTERNAL, "ZSTD compression failed: %s", ZSTD_getErrorName(r));
    return r;
  }
  if (codec == FSTX_CODEC_LZ4) {
    if (n > (size_t)LZ4_MAX_INPUT_SIZE) fail(FSTX_ERR_INTERNAL, "page too large for LZ4");
    int r = LZ4_compress_default((const char*)src, (char*)dst, (int)n, (int)std::min<size_t>(cap, 0x7fffffff));
    if (r <= 0) fail(FSTX_ERR_INTERNAL, "LZ4 compression failed");
    return (size_t)r;
  }
  fail(FSTX_ERR_INTERNAL, "unknown codec %d", codec);
}

void decompress_page(CodecContext& ctx, int codec, const void* src, size_t comp_len, void* dst, size_t raw_len) {
  if (codec == FSTX_CODEC_ZSTD) {
    if (!ctx.zd) {
      ctx.zd = ZSTD_createDCtx();
      if (!ctx.zd) fail(FSTX_ERR_MEMORY, "cannot create a ZSTD context");
    }
    size_t r = ZSTD_decompressDCtx((ZSTD_DCtx*)ctx.zd, dst, raw_len, src, comp_len);
    if (ZSTD_isError(r)) fail(FSTX_ERR_CORRUPT, "corrupt page: ZSTD error '%s'", ZSTD_getErrorName(r));
    if (r != raw_len) fail(FSTX_ERR_CORRUPT, "corrupt page: ZSTD gave %llu bytes, expected %llu", (unsigned long long)r, (unsigned long long)raw_len);
    return;
  }
  if (codec == FSTX_CODEC_LZ4) {
    if (comp_len > 0x7fffffff || raw_len > 0x7fffffff) fail(FSTX_ERR_CORRUPT, "corrupt page: LZ4 sizes out of range");
    // safe decoder only: never reads past src+comp_len or writes past dst+raw_len
    int r = LZ4_decompress_safe((const char*)src, (char*)dst, (int)comp_len, (int)raw_len);
    if (r < 0 || (size_t)r != raw_len) fail(FSTX_ERR_CORRUPT, "corrupt page: LZ4 decoding failed");
    return;
  }
  fail(FSTX_ERR_CORRUPT, "corrupt page: unknown codec %d", codec);
}

// ---------------------------------------------------------------- thread pool
ThreadPool& ThreadPool::instance() {
  // Never destroyed: joining threads during process exit can deadlock on Windows.
  // fstx_shutdown() stops the workers when the R package is unloaded.
  static ThreadPool* p = new ThreadPool();
  return *p;
}

ThreadPool::~ThreadPool() { shutdown(); }

void ThreadPool::worker_main(int id) {
  uint64_t seen = 0;
  std::unique_lock<std::mutex> lk(m_);
  for (;;) {
    cv_work_.wait(lk, [&] { return stop_ || generation_ != seen; });
    if (stop_) return;
    seen = generation_;
    if (id >= active_) continue;
    lk.unlock();
    try {
      job_(id);
    } catch (...) {
      std::lock_guard<std::mutex> g(m_);
      if (!error_) error_ = std::current_exception();
    }
    lk.lock();
    if (--remaining_ == 0) cv_done_.notify_all();
  }
}

void ThreadPool::start(int k, std::function<void(int)> job) {
  std::unique_lock<std::mutex> lk(m_);
  if (running_) fail(FSTX_ERR_INTERNAL, "worker pool is already running a job");
  if (k < 1) k = 1;
  while ((int)threads_.size() < k) {
    int id = (int)threads_.size();
    threads_.emplace_back([this, id] { worker_main(id); });
  }
  job_ = std::move(job);
  active_ = k;
  remaining_ = k;
  error_ = nullptr;
  running_ = true;
  ++generation_;
  cv_work_.notify_all();
}

bool ThreadPool::busy() {
  std::lock_guard<std::mutex> g(m_);
  return running_;
}

void ThreadPool::wait() {
  std::unique_lock<std::mutex> lk(m_);
  if (!running_) return;
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
  for (auto& t : threads_)
    if (t.joinable()) t.join();
  threads_.clear();
  std::lock_guard<std::mutex> g(m_);
  stop_ = false;
  running_ = false;
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
