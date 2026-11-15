// fstx libFuzzer harness, shared part (WS6). Not part of the package (tools/ is left out of the tarball).
//
// Input = 16 control bytes, then an fstx file. The control bytes pick options (threads, row windows, columns,
// predicates, forge fields); the rest is written to a file in /dev/shm and opened through the engine's C API.
// Before writing, fixup() puts back the header, footer and page checksums the mutation broke, so mutated bytes
// reach the footer parser and the decoders (the engine itself is not changed; reads use verify = 0). Control byte
// 0 bit 7 skips the fixup, so the checksum paths are fuzzed too.
//
// Oracles (a broken one calls fz_fail, which aborts, so libFuzzer saves the input):
//   - no crash, no sanitizer report, no hang;
//   - two ways of reading the same rows agree whenever both succeed (threads 1 / 2, SIMD on / off, whole table /
//     row window / row list / column source / filter / after compaction / after append);
//   - the format rules: an unknown required bit is refused, an unknown optional bit is read but never appended to
//     or page-copied, appends and page copies keep version and profile.
#ifndef FSTX_FUZZ_COMMON_H
#define FSTX_FUZZ_COMMON_H

#include <fcntl.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core_internal.h"
#include "fstx_core.h"
#include "fstx_footer_generated.h"
#include "simd.h"

namespace fz {

constexpr size_t kCtl = 16;
constexpr uint64_t kMaxRows = 1u << 20;  // reads of bigger declared tables are skipped (host memory, not a bug)
constexpr uint32_t kMaxCols = 64;

[[noreturn]] inline void fail(const char* what, const char* detail = "") {
  std::fprintf(stderr, "FSTX FUZZ ORACLE: %s %s\n", what, detail);
  std::abort();
}

struct Ctl {
  uint8_t b[kCtl] = {0};
  size_t pos = 0;
  uint32_t rng = 0;
  explicit Ctl(const uint8_t* d) {
    std::memcpy(b, d, kCtl);
    rng = 0x9E3779B9u ^ ((uint32_t)b[12] | (uint32_t)b[13] << 8 | (uint32_t)b[14] << 16 | (uint32_t)b[15] << 24);
  }
  bool bit(int byte, int k) const { return (b[byte] >> k & 1u) != 0; }
  uint32_t next() {  // xorshift, seeded by control bytes 12-15
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
  }
  uint64_t below(uint64_t n) { return n ? ((uint64_t)next() << 32 | next()) % n : 0; }
};

inline const fstx::fb::Footer* footer_of(const uint8_t* d, uint64_t size, uint64_t* foff_out = nullptr,
                                         uint64_t* flen_out = nullptr) {
  if (size < 96) return nullptr;
  uint64_t end = size, ve = fstx::get_u64(d + 16);
  if (ve && ve <= size && ve >= 96) end = ve;
  const uint8_t* tail = d + end - 32;
  const uint64_t foff = fstx::get_u64(tail), flen = fstx::get_u64(tail + 8);
  if (foff < 64 || foff % 8 || foff > end - 32 || flen < 8 || flen > end - 32 - foff || flen >= 0x7fffffffULL)
    return nullptr;
  if (foff_out) *foff_out = foff;
  if (flen_out) *flen_out = flen;
  flatbuffers::Verifier::Options o;
  o.max_depth = 16;
  o.max_tables = 0x7fffffff;
  o.check_alignment = true;
  flatbuffers::Verifier v(d + foff, (size_t)flen, o);
  if (!fstx::fb::VerifyFooterBuffer(v)) return nullptr;
  return fstx::fb::GetFooter(d + foff);
}

template <class F>
inline void each_page(const fstx::fb::Footer* ft, F fn) {
  if (!ft->row_groups()) return;
  for (const auto* rg : *ft->row_groups()) {
    if (!rg->columns()) continue;
    uint32_t c = 0;
    for (const auto* ch : *rg->columns()) {
      if (ch->dictionary()) fn(c, ch->dictionary(), true);
      if (ch->pages())
        for (const auto* p : *ch->pages()) fn(c, p, false);
      ++c;
    }
  }
}

// Puts back the checksums: page hashes inside the footer, then the footer hash in the tail, then the header hash.
// clear_stats also clears every page's "min / max valid" flag.
inline void fixup(std::vector<uint8_t>& b, bool clear_stats) {
  if (b.size() < 96) return;
  uint8_t* d = b.data();
  uint64_t foff = 0, flen = 0;
  if (const auto* ft = footer_of(d, b.size(), &foff, &flen)) {
    each_page(ft, [&](uint32_t, const fstx::fb::Page* p, bool) {
      // raw bytes, not p->offset(): a mutated footer can put the Page vector off its 8-byte alignment (the
      // FlatBuffers verifier checks only the vector's 4-byte length), and a member call there is UB
      uint8_t* s = (uint8_t*)p;  // the Page struct sits inside our own buffer
      if (clear_stats) s[61] &= (uint8_t)~1u;
      const uint64_t off = fstx::get_u64(s), cl = fstx::get_u32(s + 40);
      if (off <= b.size() && cl <= b.size() - off) fstx::put_u64(s + 16, fstx::hash64(d + off, (size_t)cl));
    });
    uint64_t end = b.size(), ve = fstx::get_u64(d + 16);
    if (ve && ve <= b.size() && ve >= 96) end = ve;
    fstx::put_u64(d + end - 32 + 16, fstx::hash64(d + foff, (size_t)flen));
  } else {
    uint64_t end = b.size(), ve = fstx::get_u64(d + 16);
    if (ve && ve <= b.size() && ve >= 96) end = ve;
    const uint8_t* tail = d + end - 32;
    const uint64_t fo = fstx::get_u64(tail), fl = fstx::get_u64(tail + 8);
    if (fo <= end && fl <= end - fo) fstx::put_u64(d + end - 32 + 16, fstx::hash64(d + fo, (size_t)fl));
  }
  fstx::put_u64(d + 24, fstx::hash64(d, 24));
}

inline std::string tmp_path(const char* tag) {
  char buf[128];
  std::snprintf(buf, sizeof buf, "/dev/shm/fstxfz_%s_%d.fstx", tag, (int)getpid());
  return buf;
}

inline bool write_file(const std::string& path, const uint8_t* d, size_t n) {
  int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (fd < 0) return false;
  size_t off = 0;
  while (off < n) {
    ssize_t w = ::write(fd, d + off, n - off);
    if (w <= 0) break;
    off += (size_t)w;
  }
  ::close(fd);
  return off == n;
}

inline std::vector<uint8_t> read_file(const std::string& path) {
  std::vector<uint8_t> v;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return v;
  uint8_t buf[65536];
  size_t k;
  while ((k = std::fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + k);
  std::fclose(f);
  return v;
}

// The fuzz input as a file: the control bytes are cut off, the checksums fixed (unless control says not to).
// FSTX_FUZZ_DUMP=<path> also writes the fixed file there (to make a reproducer for the R tests).
inline std::vector<uint8_t> file_bytes(const uint8_t* data, size_t size, const Ctl& c) {
  std::vector<uint8_t> b(data + kCtl, data + size);
  if (!c.bit(0, 7)) fixup(b, c.bit(0, 6));
  if (const char* dump = std::getenv("FSTX_FUZZ_DUMP")) write_file(dump, b.data(), b.size());
  return b;
}

// ---------------------------------------------------------------- a decoded table
struct Col {
  int phys = 0;
  std::vector<int32_t> i;   // INT32, LOGICAL
  std::vector<double> d;    // FLOAT64
  std::vector<std::string> s;
  std::vector<uint8_t> na;  // STRING: 1 = NA
  std::vector<std::vector<std::string>> dicts;  // per output column: the current dictionary
};

struct Table {
  std::vector<Col> cols;
  uint64_t n = 0;
  int phys_of(size_t j) const { return cols[j].phys; }
};

inline bool same_dbl(double a, double b) { return std::memcmp(&a, &b, 8) == 0; }

// row r of column j of t equals row q of column k of u
inline bool same_cell(const Table& t, size_t j, uint64_t r, const Table& u, size_t k, uint64_t q) {
  const Col& a = t.cols[j];
  const Col& b = u.cols[k];
  if (a.phys != b.phys) return false;
  switch (a.phys) {
    case FSTX_FLOAT64: return same_dbl(a.d[r], b.d[q]);
    case FSTX_STRING: return a.na[r] == b.na[q] && (a.na[r] || a.s[r] == b.s[q]);
    default: return a.i[r] == b.i[q];
  }
}

struct Host {
  Table* t;
  std::vector<std::vector<std::string>> dict;
};

inline void* h_alloc(void* ctx, uint32_t j, uint32_t, int phys, uint64_t n) {
  Host* h = (Host*)ctx;
  if (n > kMaxRows) return nullptr;
  Col& c = h->t->cols[j];
  c.phys = phys;
  if (phys == FSTX_FLOAT64) {
    c.d.assign(n, 0.0);
    return c.d.data();
  }
  if (phys == FSTX_STRING) {
    c.s.assign(n, std::string());
    c.na.assign(n, 2);  // 2 = never delivered
    return &c;
  }
  c.i.assign(n, 0);
  return c.i.data();
}

inline void put_str(Col& c, uint64_t row, int32_t len, const char* p) {
  if (row >= c.s.size()) fail("string delivered outside the output");
  if (len < 0) {
    c.na[row] = 1;
    c.s[row].clear();
  } else {
    c.na[row] = 0;
    c.s[row].assign(p, (size_t)len);
  }
}

inline int h_strings(void* ctx, uint32_t j, uint64_t dest, uint32_t n, const int32_t* lens, const char* bytes) {
  Col& c = ((Host*)ctx)->t->cols[j];
  for (uint32_t i = 0; i < n; ++i) {
    if (lens[i] < -1) fail("string length below -1 handed to the host");
    put_str(c, dest + i, lens[i], bytes);
    if (lens[i] > 0) bytes += lens[i];
  }
  return 0;
}

inline int h_dict(void* ctx, uint32_t j, uint32_t n, const int32_t* lens, const char* bytes) {
  Host* h = (Host*)ctx;
  if (h->dict.size() <= j) h->dict.resize(j + 1);
  auto& D = h->dict[j];
  D.clear();
  for (uint32_t i = 0; i < n; ++i) {
    if (lens[i] < 0) fail("NA inside a string dictionary handed to the host");
    D.emplace_back(bytes, (size_t)lens[i]);
    bytes += lens[i];
  }
  return 0;
}

inline int h_codes(void* ctx, uint32_t j, uint64_t dest, uint32_t n, const int32_t* codes) {
  Host* h = (Host*)ctx;
  Col& c = h->t->cols[j];
  if (h->dict.size() <= j) fail("codes before any dictionary");
  const auto& D = h->dict[j];
  for (uint32_t i = 0; i < n; ++i) {
    if (codes[i] < -1 || codes[i] >= (int32_t)D.size()) fail("dictionary code out of range handed to the host");
    put_str(c, dest + i, codes[i] < 0 ? -1 : (int32_t)D[(size_t)codes[i]].size(),
            codes[i] < 0 ? nullptr : D[(size_t)codes[i]].data());
  }
  return 0;
}

inline int h_strings_at(void* ctx, uint32_t j, uint32_t n, const uint64_t* dest, const int32_t* lens,
                        const char* bytes) {
  Col& c = ((Host*)ctx)->t->cols[j];
  for (uint32_t i = 0; i < n; ++i) {
    if (lens[i] < -1) fail("string length below -1 handed to the host");
    put_str(c, dest[i], lens[i], bytes);
    if (lens[i] > 0) bytes += lens[i];
  }
  return 0;
}

// Reads with plan p into t (t.cols sized to the plan's columns). Returns the engine's code.
inline int run_plan(const fstx_plan* p, uint32_t ncols, Table& t, int threads, int verify) {
  t.cols.assign(ncols, Col());
  t.n = fstx_plan_rows(p);
  Host h{&t, {}};
  fstx_read_host host{&h, h_alloc, h_strings, h_dict, h_codes, h_strings_at};
  fstx_error err;
  uint64_t a = 0, b = 0;
  fstx_plan_string_stats(p, &a, &b);
  const int rc = fstx_read(p, &host, threads, verify, &err);
  if (rc == FSTX_OK) {
    for (const Col& c : t.cols)
      if (c.phys == FSTX_STRING)
        for (uint8_t x : c.na)
          if (x == 2) fail("read succeeded but a string row was never delivered");
  }
  return rc;
}

// Whole table, every column (at most kMaxCols), rows [from, to).
inline int read_range(const fstx_file* f, uint64_t from, uint64_t to, Table& t, int threads, int verify,
                      const std::vector<uint32_t>* cols = nullptr) {
  fstx_file_info fi;
  fstx_file_info_get(f, &fi);
  std::vector<uint32_t> all;
  if (!cols) {
    for (uint32_t c = 0; c < fi.n_cols && c < kMaxCols; ++c) all.push_back(c);
    cols = &all;
  }
  fstx_plan* p = nullptr;
  fstx_error err;
  int rc = fstx_plan_create(f, cols->data(), (uint32_t)cols->size(), from, to, &p, &err);
  if (rc != FSTX_OK) return rc;
  rc = run_plan(p, (uint32_t)cols->size(), t, threads, verify);
  fstx_plan_free(p);
  return rc;
}

inline bool table_small(const fstx_file* f) {
  fstx_file_info fi;
  fstx_file_info_get(f, &fi);
  return fi.n_rows <= kMaxRows && fi.n_cols <= kMaxCols;
}

inline void must_equal(const Table& a, const Table& b, const char* what) {
  if (a.n != b.n || a.cols.size() != b.cols.size()) fail(what, "(shape)");
  for (size_t j = 0; j < a.cols.size(); ++j)
    for (uint64_t r = 0; r < a.n; ++r)
      if (!same_cell(a, j, r, b, j, r)) fail(what, "(value)");
}

// Feature fields of a file's footer (from our own bytes), for the format-rule oracles.
struct Facts {
  bool ok = false;
  uint32_t version = 0;
  int profile = 1;
  uint64_t req = 0, opt = 0;
  uint32_t n_names = 0;
};

inline Facts facts_of(const std::vector<uint8_t>& b) {
  Facts x;
  const auto* ft = footer_of(b.data(), b.size());
  if (!ft) return x;
  x.ok = true;
  x.version = ft->format_version();
  x.profile = (int)ft->profile();
  x.req = ft->required_features();
  x.opt = ft->optional_features();
  x.n_names = ft->feature_names() ? ft->feature_names()->size() : 0;
  return x;
}

}  // namespace fz

#endif
