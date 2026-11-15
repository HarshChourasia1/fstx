// FST-X core engine: internal declarations shared by the core_*.cpp files. No R headers.
#ifndef FSTX_CORE_INTERNAL_H
#define FSTX_CORE_INTERNAL_H

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "codec_ext.h"
#include "fstx_core.h"
#include "prof.h"

namespace fstx {

// ---------------------------------------------------------------- version and format constants
// The package version, from DESCRIPTION through configure (-DFSTX_VERSION=x.y.z); written into created_by.
#ifndef FSTX_VERSION
#define FSTX_VERSION unknown
#endif
#define FSTX_STR_(x) #x
#define FSTX_STR(x) FSTX_STR_(x)
constexpr const char* kVersion = FSTX_STR(FSTX_VERSION);

constexpr uint32_t kFormatVersion = 3;     // read and written ("fstx format 1.0"); files of fstx 0.x are refused
constexpr size_t kHeaderSize = 32;
constexpr size_t kTailSize = 32;
constexpr uint64_t kAlign = 64;           // every page (and the footer) starts on a 64-byte boundary
constexpr uint64_t kDataStart = 64;       // first page offset (header + padding)
constexpr uint32_t kEndianMark = 0x01020304u;
constexpr uint64_t kDefaultRowGroup = 1048576;
constexpr uint32_t kDefaultPageBytes = 262144;
constexpr uint32_t kVector = 1024;        // page row counts are multiples of this (except the last / long strings)
constexpr uint64_t kMaxRawPage = 0x7fffffffULL;  // LZ4 and int32 string lengths need raw pages below 2 GB
constexpr uint8_t kFlagMinMax = 1;
// Limits a reader enforces (the writer stays inside them): see inst/spec/fstx-format-1.0.md.
constexpr uint32_t kMaxColumns = 1u << 24;
constexpr uint64_t kMaxRowGroup = 1ull << 31;   // rows per row group (footer row_group_size)
constexpr uint32_t kMaxDictEntries = 1u << 25;  // entries in one string dictionary page

// Feature bits (format 3: footer required_features / optional_features). Bits 0-15 are for features that change
// how the header, tail or footer are read; they are also in header bytes 6-7, so a reader refuses them before it
// reads the footer. Bits 16-63 are for everything else (types, codecs, encodings). This build uses none; the
// named ones are reserved for later versions.
enum : int { kFeatInt64 = 16, kFeatBinary = 17, kFeatZstdDict = 18, kFeatPcodec = 19, kFeatExperimental = 63 };
constexpr uint64_t kFeatHeaderMask = 0xffffu;
const char* feature_name(int bit);  // reserved name of a bit, or nullptr
uint64_t features_known();          // bits this build can read
bool valid_utf8(const char* s, size_t n);  // well-formed UTF-8 (no overlongs, surrogates or values past U+10FFFF)

// ---------------------------------------------------------------- errors
struct Error : public std::exception {
  int code;
  int64_t column;
  std::string msg;
  Error(int c, std::string m, int64_t col = -1) : code(c), column(col), msg(std::move(m)) {}
  const char* what() const noexcept override { return msg.c_str(); }
};

[[noreturn]]  // standard attribute first: clang rejects it after a GNU __attribute__
#if defined(__MINGW32__)
__attribute__((format(gnu_printf, 2, 3)))
#elif defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void fail(int code, const char* fmt, ...);
void set_error(fstx_error* err, int code, const char* msg, int64_t column = -1);

// ---------------------------------------------------------------- little-endian helpers
inline bool host_is_little_endian() {
  const uint32_t x = 1;
  unsigned char c;
  std::memcpy(&c, &x, 1);
  return c == 1;
}
inline void put_u16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
inline void put_u32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
inline void put_u64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }
inline uint16_t get_u16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint32_t get_u32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t get_u64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint64_t align_up(uint64_t x) { return (x + kAlign - 1) & ~(kAlign - 1); }

// ---------------------------------------------------------------- hashing (XXH3-64)
uint64_t hash64(const void* p, size_t n);

// ---------------------------------------------------------------- files
// Identity of a file on disk (volume + file index on Windows, device + inode on POSIX), so two spellings of
// one path compare equal.
struct FileId {
  uint64_t a = 0, b = 0;
  bool ok = false;
  bool operator<(const FileId& o) const { return a < o.a || (a == o.a && b < o.b); }
};
FileId file_id_of_path(const std::string& path_utf8);  // ok = false when the file does not exist
// Files open in this process (fstx_open .. fstx_close). The writer refuses to replace one of them.
void registry_add(const FileId& id);
void registry_remove(const FileId& id);
bool registry_has(const FileId& id);

// Converts the current exception into err; returns the error code.
int to_error(fstx_error* err);

// Read-only memory map of a whole file. Opened with CreateFileW on Windows.
class MappedFile {
 public:
  MappedFile() = default;
  ~MappedFile();
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  void open(const std::string& path_utf8);
  const uint8_t* data() const { return data_; }
  uint64_t size() const { return size_; }
  // Drops the OS pages under [off, off + n) from the process working set once a page is decoded. The data
  // stays mapped (a later touch is a soft fault from the file cache), so this only lowers reported RAM.
  void release(uint64_t off, uint64_t n) const;
  const FileId& id() const { return id_; }

 private:
  FileId id_;
  const uint8_t* data_ = nullptr;
  uint64_t size_ = 0;
#ifdef _WIN32
  void* file_ = nullptr;
  void* mapping_ = nullptr;
#else
  int fd_ = -1;
#endif
};

// Sequential writer with an explicit position.
class OutFile {
 public:
  OutFile() = default;
  ~OutFile();
  OutFile(const OutFile&) = delete;
  OutFile& operator=(const OutFile&) = delete;
  void create(const std::string& path_utf8);
  void write(const void* p, size_t n);
  void pad_to(uint64_t pos);  // zero bytes up to pos
  uint64_t pos() const { return pos_; }
  void close();               // flushes; throws on failure
  void close_and_delete();    // never throws (an existing file opened with open_existing is kept)
  bool is_open() const;
  // M5 appends: opens an existing file for writing at its end (refused while the file is open in this
  // session); nothing is deleted on failure.
  void open_existing(const std::string& path_utf8);
  void write_at(uint64_t off, const void* p, size_t n);  // flushes, writes at off, then resumes at pos()
  void truncate(uint64_t size);                         // flushes; the file ends at size and writing resumes there
  void sync();                                          // flushes to the disk (FlushFileBuffers / fsync)

 private:
  void flush_buffer();
  std::string path_;
  uint64_t pos_ = 0;
  bool existing_ = false;
  std::vector<uint8_t> buf_;  // small writes are batched here; big ones go straight through
#ifdef _WIN32
  void* file_ = nullptr;
#else
  FILE* file_ = nullptr;
#endif
};

// ---------------------------------------------------------------- codecs: codec_ext.h

// ---------------------------------------------------------------- thread pool
// Persistent worker pool. start() hands a job to k workers and returns at once, so the calling thread
// can do its own part (the writer thread on write, building strings on read); wait() joins the job.
class ThreadPool {
 public:
  static ThreadPool& instance();
  void start(int k, std::function<void(int)> job);
  bool busy();  // a job is running (callers then work on their own thread)
  void wait();  // rethrows the first exception a worker raised
  void shutdown();
  ~ThreadPool();

 private:
  void worker_main(int id);
  std::mutex m_;
  std::condition_variable cv_work_, cv_done_;
  std::vector<std::thread> threads_;
  std::function<void(int)> job_;
  uint64_t generation_ = 0;
  int active_ = 0;
  int remaining_ = 0;
  bool stop_ = false;
  bool running_ = false;
  std::exception_ptr error_;
};

int resolve_threads(int threads);

}  // namespace fstx

#endif
