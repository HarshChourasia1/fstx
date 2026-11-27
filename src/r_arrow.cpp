// FST-X R layer, 1.0: Arrow C stream straight from the file (WS2). MIT licence.
// The stream is built on the public engine calls of fstx_core.h (column sources), one batch per row group (or
// smaller, batch_rows). Its callbacks are plain C++ and never call R: DuckDB, Polars or arrow may call them from
// their own threads. Errors come back through the stream's error slot. Range batches: every string page and every
// row part of a fixed-width column decodes as a task on the engine pool; fixed-width columns decode straight into
// the Arrow buffers, string pages are then copied once into them, page by page in parallel (batch_range).
// Row-list batches: arrow_rows.h.
// Everything above the R glue at the bottom builds without R (-DFSTX_ARROW_NO_R, for the fuzz target).
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "arrow_abi.h"

namespace {

std::atomic<int64_t> g_live{0};  // live stream / schema / array holders (the tests check it returns to its start)
// The engine has one worker pool that runs one job at a time: batches of different streams take turns.
std::mutex g_engine;

struct Live {
  Live() { ++g_live; }
  ~Live() { --g_live; }
  Live(const Live&) = delete;
  Live& operator=(const Live&) = delete;
};

struct Fail : std::runtime_error {
  using std::runtime_error::runtime_error;
};
void check(int rc, const fstx_error& e) {
  if (rc != FSTX_OK) throw Fail(e.message);
}

void* xalloc(size_t n) {
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}

bool valid_utf8(const uint8_t* s, size_t n) {
  size_t i = 0;
  while (i < n) {
    for (; i + 16 <= n; i += 16) {  // ASCII runs, 16 bytes at a time
      uint64_t x, y;
      std::memcpy(&x, s + i, 8);
      std::memcpy(&y, s + i + 8, 8);
      if ((x | y) & 0x8080808080808080ull) break;
    }
    if (i >= n) break;
    const uint8_t c = s[i];
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t k;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) k = 1, cp = c & 0x1F;
    else if ((c & 0xF0) == 0xE0) k = 2, cp = c & 0x0F;
    else if ((c & 0xF8) == 0xF0) k = 3, cp = c & 0x07;
    else return false;
    if (n - i <= k) return false;
    for (size_t j = 1; j <= k; ++j) {
      if ((s[i + j] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (s[i + j] & 0x3F);
    }
    if ((k == 1 && cp < 0x80) || (k == 2 && cp < 0x800) || (k == 3 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    i += k + 1;
  }
  return true;
}

// ---------------------------------------------------------------- schemas
struct SchemaHold : Live {
  std::string format, name;
  std::vector<ArrowSchema*> kids;
  ArrowSchema* dict = nullptr;
};

void schema_release(ArrowSchema* s) {
  SchemaHold* h = (SchemaHold*)s->private_data;
  for (ArrowSchema* k : h->kids) {
    if (k->release) k->release(k);
    delete k;
  }
  if (h->dict) {
    if (h->dict->release) h->dict->release(h->dict);
    delete h->dict;
  }
  delete h;
  s->release = nullptr;
}

SchemaHold* schema_init(ArrowSchema* s, const std::string& format, const std::string& name, int64_t flags) {
  SchemaHold* h = new SchemaHold();
  h->format = format;
  h->name = name;
  std::memset(s, 0, sizeof(*s));
  s->format = h->format.c_str();
  s->name = h->name.c_str();
  s->flags = flags;
  s->release = schema_release;
  s->private_data = h;
  return h;
}

// ---------------------------------------------------------------- arrays
struct ArrayHold : Live {
  std::vector<void*> mem;
  const void* bufs[3] = {nullptr, nullptr, nullptr};
  std::vector<ArrowArray*> kids;
  ArrowArray* dict = nullptr;
};

void array_release(ArrowArray* a) {
  ArrayHold* h = (ArrayHold*)a->private_data;
  for (ArrowArray* k : h->kids) {
    if (k->release) k->release(k);
    delete k;
  }
  if (h->dict) {
    if (h->dict->release) h->dict->release(h->dict);
    delete h->dict;
  }
  for (void* p : h->mem) std::free(p);
  delete h;
  a->release = nullptr;
}

ArrayHold* array_init(ArrowArray* a, int64_t length, int n_buffers) {
  ArrayHold* h = new ArrayHold();
  std::memset(a, 0, sizeof(*a));
  a->length = length;
  a->n_buffers = n_buffers;
  a->buffers = h->bufs;
  a->release = array_release;
  a->private_data = h;
  return h;
}

void* array_buf(ArrowArray* a, int i, size_t bytes) {
  ArrayHold* h = (ArrayHold*)a->private_data;
  h->mem.push_back(nullptr);
  void* p = xalloc(bytes);
  h->mem.back() = p;
  h->bufs[i] = p;
  return p;
}

ArrowArray* new_child() {
  ArrowArray* c = new ArrowArray();
  std::memset(c, 0, sizeof(*c));
  return c;
}

// Validity bitmap made on the first NA (none for columns without one).
struct Validity {
  ArrowArray* a;
  uint64_t n;
  uint8_t* bits = nullptr;
  int64_t nulls = 0;
  void null_at(uint64_t i) {
    if (!bits) {
      bits = (uint8_t*)array_buf(a, 0, (size_t)((n + 7) / 8));
      std::memset(bits, 0xFF, (size_t)((n + 7) / 8));
    }
    bits[i >> 3] &= (uint8_t) ~(1u << (i & 7));
    ++nulls;
  }
  void done() { a->null_count = nulls; }
};

// A utf8 array of n strings that are never NA, laid out as lengths plus bytes back to back.
void make_utf8(ArrowArray* a, uint64_t n, const int32_t* lens, const char* bytes) {
  array_init(a, (int64_t)n, 3);
  int32_t* off = (int32_t*)array_buf(a, 1, (size_t)(n + 1) * 4);
  uint64_t tot = 0;
  off[0] = 0;
  for (uint64_t i = 0; i < n; ++i) {
    tot += (uint64_t)std::max(0, lens[i]);
    if (tot > (uint64_t)INT32_MAX) throw Fail("a dictionary holds more than 2 GB of text");
    off[i + 1] = (int32_t)tot;
  }
  void* d = array_buf(a, 2, (size_t)tot);
  if (tot) std::memcpy(d, bytes, (size_t)tot);
}

// ---------------------------------------------------------------- the stream
enum Out { O_INT32, O_FLOAT64, O_BOOL, O_UTF8, O_LARGE_UTF8, O_DICT_STR, O_FACTOR, O_DATE, O_TS };

struct ColSpec {
  uint32_t col = 0;
  std::string name;
  int out = O_INT32;
  int phys = FSTX_INT32;
  bool ordered = false;
  std::string tz;
  bool has_tz = false;
  std::vector<int32_t> lev_lens;  // factor levels
  std::string lev_bytes;
};

struct SrcFree {
  void operator()(fstx_colsrc* s) const { fstx_colsrc_free(s); }
};
using SrcPtr = std::unique_ptr<fstx_colsrc, SrcFree>;

struct Stream : Live {
  fstx_file* f = nullptr;
  std::vector<ColSpec> cols;
  bool by_rows = false;
  uint64_t from = 0, to = 0;
  std::vector<uint64_t> rows;
  uint64_t pos = 0;               // next row (range) or next index into rows
  std::vector<uint64_t> rg_ends;  // end row of each row group, ascending
  int threads = 1;
  uint64_t batch_rows = 0;
  int verify = 1;
  bool plain = false;              // FSTX_ARROW_PLAIN_STRINGS
  uint8_t* utf8_bad = nullptr;     // FSTX_ARROW_CHECK_UTF8: the caller's flags, one per column
  std::vector<fstx_bufpool*> bufs;  // per column: string page buffers kept from one batch for the next
  fstx_scratchset* scratch = nullptr;  // the workers' codec contexts and page buffers, kept between batches
  std::string error;
  std::shared_ptr<void> rows_all;  // arrow_rows.h: a selection decoded once for all its batches
  ~Stream() {
    fstx_scratchset_free(scratch);
    for (fstx_bufpool* p : bufs) fstx_bufpool_free(p);
    if (f) fstx_close(f);
  }
};

uint64_t rg_end_of(const Stream& S, uint64_t row) {
  auto it = std::upper_bound(S.rg_ends.begin(), S.rg_ends.end(), row);
  return it == S.rg_ends.end() ? S.rg_ends.back() : *it;
}

std::string schema_format(const ColSpec& c) {
  switch (c.out) {
    case O_INT32: return "i";
    case O_FLOAT64: return "g";
    case O_BOOL: return "b";
    case O_UTF8: return "u";
    case O_LARGE_UTF8: return "U";
    case O_DICT_STR:
    case O_FACTOR: return "i";
    case O_DATE: return "tdD";
    default: return "tsu:" + c.tz;
  }
}

SrcPtr open_src(const Stream& S, const ColSpec& c, uint64_t a, uint64_t b) {
  fstx_colsrc* s = nullptr;
  fstx_error e;
  check(fstx_colsrc_open(S.f, c.col, a, b, S.verify, 0, &s, &e), e);
  return SrcPtr(s);
}

// A fixed-width column (int, double, logical, factor codes, Date, POSIXct) of a range batch over file rows [a, b)
// (row-list batches: arrow_rows.h). fixed_prep sets up its Arrow array on the calling thread; then
// fixed_part fills output rows [i0, i1), one call or several at once on different threads. Parts split at page
// edges that fall on a whole byte of the bitmaps, so no page is decoded twice and no byte is written by two parts.
struct FixedCol {
  const ColSpec* c = nullptr;
  ArrowArray* out = nullptr;
  uint64_t a = 0, n = 0;
  bool same = false;  // the Arrow values are the stored values: decoded straight into the Arrow buffer
  bool scan = true;   // look for NAs (straight decode: only when a page of the batch has any, as before the split)
  void* vals = nullptr;
  std::mutex m;  // makes the validity bitmap on the first NA of any part
  uint8_t* bits = nullptr;
  std::atomic<int64_t> nulls{0};
  uint8_t* bitmap() {
    std::lock_guard<std::mutex> g(m);
    if (!bits) {
      uint8_t* p = (uint8_t*)array_buf(out, 0, (size_t)((n + 7) / 8));
      std::memset(p, 0xFF, (size_t)((n + 7) / 8));
      bits = p;
    }
    return bits;
  }
};

// Returns the part boundaries (output rows, from 0 to n), at most `parts` parts.
std::vector<uint64_t> fixed_prep(const Stream& S, FixedCol& F, uint64_t b, int parts) {
  const ColSpec& c = *F.c;
  const uint64_t n = F.n;
  SrcPtr s = open_src(S, c, F.a, b);
  fstx_colsrc_info si;
  fstx_colsrc_info_get(s.get(), &si);
  F.same = (c.out == O_INT32 || c.out == O_FLOAT64 || (c.out == O_DATE && c.phys == FSTX_INT32));
  F.scan = !F.same || si.null_count;
  array_init(F.out, (int64_t)n, 2);
  const size_t ow = c.out == O_BOOL ? 0 : (c.out == O_FLOAT64 || c.out == O_TS) ? 8 : 4;
  if (ow) {
    F.vals = array_buf(F.out, 1, (size_t)n * ow);
  } else {
    F.vals = array_buf(F.out, 1, (size_t)((n + 7) / 8));
    std::memset(F.vals, 0, (size_t)((n + 7) / 8));
  }
  if (c.out == O_FACTOR) {
    ArrayHold* h = (ArrayHold*)F.out->private_data;
    h->dict = new_child();
    F.out->dictionary = h->dict;
    make_utf8(h->dict, c.lev_lens.size(), c.lev_lens.data(), c.lev_bytes.data());
  }
  std::vector<uint64_t> cut{0};
  if (parts > 1) {
    std::vector<uint64_t> edges;  // page starts as output rows, inside (0, n), on a multiple of 8
    for (uint32_t q = 1; q < si.n_pages; ++q) {
      uint64_t first = 0;
      uint32_t k = 0;
      fstx_colsrc_page_span(s.get(), q, &first, &k);
      const uint64_t i = first;
      if (i > 0 && i < n && i % 8 == 0 && (edges.empty() || i > edges.back())) edges.push_back(i);
    }
    for (int p = 1; p < parts; ++p) {  // the first edge at or past each p/parts of the rows
      auto it = std::lower_bound(edges.begin(), edges.end(), n * (uint64_t)p / (uint64_t)parts);
      if (it != edges.end() && *it > cut.back()) cut.push_back(*it);
    }
  }
  cut.push_back(n);
  return cut;
}

void fixed_part(const Stream& S, FixedCol& F, uint64_t i0, uint64_t i1, int threads, fstx_scratch* ws = nullptr) {
  const ColSpec& c = *F.c;
  const uint64_t lo = i0, hi = i1;  // file rows, relative to a
  SrcPtr s = open_src(S, c, F.a + lo, F.a + hi);
  fstx_error e;
  const size_t w = c.phys == FSTX_FLOAT64 ? 8 : 4;
  uint8_t* bits = nullptr;
  int64_t nulls = 0;
  auto null_at = [&](uint64_t i) {
    if (!bits) bits = F.bitmap();
    bits[i >> 3] &= (uint8_t) ~(1u << (i & 7));
    ++nulls;
  };
  auto load = [&](void* dst) {  // with the task's decode state when it runs on the pool
    check(ws ? fstx_colsrc_load_with(s.get(), dst, ws, &e) : fstx_colsrc_load(s.get(), dst, threads, &e), e);
  };
  if (F.same) {  // decode straight into the Arrow buffer
    void* dst = (uint8_t*)F.vals + (size_t)i0 * w;
    load(dst);
    if (F.scan) {
      if (w == 4) {
        const int32_t* x = (const int32_t*)F.vals;
        for (uint64_t i = i0; i < i1; ++i)
          if (x[i] == std::numeric_limits<int32_t>::min()) null_at(i);
      } else {
        const uint64_t* x = (const uint64_t*)F.vals;  // R's NA_real_: a NaN whose low word is 1954; other NaN stay NaN
        for (uint64_t i = i0; i < i1; ++i)
          if ((x[i] & 0xFFFFFFFFull) == 1954 && (x[i] & 0x7FF0000000000000ull) == 0x7FF0000000000000ull)
            null_at(i);
      }
    }
    F.nulls += nulls;
    return;
  }
  std::vector<uint8_t> own;  // the values as stored, in the worker's kept buffer when there is one
  uint8_t* tmp;
  if (ws) {
    tmp = (uint8_t*)fstx_scratch_buffer(ws, (size_t)(hi - lo) * w);
    if (!tmp) throw std::bad_alloc();
  } else {
    own.resize((size_t)(hi - lo) * w);
    tmp = own.data();
  }
  load(tmp);
  s.reset();
  auto at = [&](uint64_t i) { return i - lo; };
  if (c.phys == FSTX_FLOAT64) {
    const double* x = (const double*)tmp;
    auto is_na = [](double d) {
      uint64_t u;
      std::memcpy(&u, &d, 8);
      return std::isnan(d) && (u & 0xFFFFFFFFull) == 1954;
    };
    if (c.out == O_FLOAT64) {
      double* o = (double*)F.vals;
      for (uint64_t i = i0; i < i1; ++i) {
        o[i] = x[at(i)];
        if (is_na(o[i])) null_at(i);
      }
    } else if (c.out == O_DATE) {  // whole days, rounded down (as R prints a fractional Date)
      int32_t* o = (int32_t*)F.vals;
      for (uint64_t i = i0; i < i1; ++i) {
        const double d = std::floor(x[at(i)]);
        if (!std::isfinite(d)) {
          o[i] = 0;
          null_at(i);
        } else if (d < (double)INT32_MIN || d > (double)INT32_MAX) {
          throw Fail("column '" + c.name + "' has a Date outside the range of Arrow date32");
        } else {
          o[i] = (int32_t)d;
        }
      }
    } else {  // O_TS: seconds -> microseconds, rounded to the nearest
      int64_t* o = (int64_t*)F.vals;
      for (uint64_t i = i0; i < i1; ++i) {
        const double us = std::nearbyint(x[at(i)] * 1e6);
        if (!std::isfinite(us)) {
          o[i] = 0;
          null_at(i);
        } else if (us < -9.2e18 || us > 9.2e18) {
          throw Fail("column '" + c.name + "' has a time outside the range of Arrow microsecond timestamps");
        } else {
          o[i] = (int64_t)us;
        }
      }
    }
  } else {
    const int32_t* x = (const int32_t*)tmp;
    const int32_t na = std::numeric_limits<int32_t>::min();
    if (c.out == O_BOOL) {
      uint8_t* o = (uint8_t*)F.vals;
      for (uint64_t i = i0; i < i1; ++i) {
        const int32_t k = x[at(i)];
        if (k == na) null_at(i);
        else if (k) o[i >> 3] |= (uint8_t)(1u << (i & 7));
      }
    } else {  // int32, int Date, factor codes (1-based -> 0-based dictionary indexes)
      int32_t* o = (int32_t*)F.vals;
      const int32_t shift = c.out == O_FACTOR ? 1 : 0;
      for (uint64_t i = i0; i < i1; ++i) {
        const int32_t k = x[at(i)];
        if (k == na) {
          o[i] = 0;
          null_at(i);
        } else {
          o[i] = k - shift;
        }
      }
    }
  }
  F.nulls += nulls;
}

// A string dictionary, with the byte offset of every entry (dictionary pages laid out as plain strings).
struct DictTab {
  uint32_t n = 0;
  const int32_t* lens = nullptr;
  const char* bytes = nullptr;
  std::vector<uint64_t> off;
  uint64_t tot = 0;  // bytes of text
  bool bad = false;  // not valid UTF-8 (checked only when asked)
};

DictTab dict_tab(fstx_colsrc* s, uint32_t d) {
  DictTab t;
  fstx_error e;
  check(fstx_colsrc_dict(s, d, &t.n, &t.lens, &t.bytes, &e), e);
  t.off.resize(t.n);
  uint64_t o = 0;
  for (uint32_t i = 0; i < t.n; ++i) {
    t.off[i] = o;
    o += (uint64_t)std::max(0, t.lens[i]);
  }
  t.tot = o;
  return t;
}

// A dictionary-coded character column of a range batch (pages loaded), as dictionary indexes into the row group's
// dictionary (its one dictionary: a batch never spans row groups).
void dict_column(const ColSpec& c, fstx_colsrc* s, uint64_t n, ArrowArray* out) {
  fstx_error e;
  fstx_colsrc_info si;
  fstx_colsrc_info_get(s, &si);
  if (si.n_dicts != 1) throw Fail("internal error: a batch spans more than one dictionary");
  array_init(out, (int64_t)n, 2);
  Validity v{out, n};
  int32_t* o = (int32_t*)array_buf(out, 1, (size_t)n * 4);
  for (uint32_t q = 0; q < si.n_pages; ++q) {
    const int32_t* pl = nullptr;
    const char* pb = nullptr;
    int32_t d = -1;
    check(fstx_colsrc_str_page(s, q, &pl, &pb, &d, &e), e);
    if (d < 0) throw Fail("internal error: dictionary and plain pages mixed in column '" + c.name + "'");
    uint64_t first = 0;
    uint32_t k = 0;
    fstx_colsrc_page_span(s, q, &first, &k);
    for (uint32_t j = 0; j < k; ++j) {
      o[first + j] = pl[j] < 0 ? 0 : pl[j];
      if (pl[j] < 0) v.null_at(first + j);
    }
  }
  v.done();
  uint32_t dn = 0;
  const int32_t* dl = nullptr;
  const char* db = nullptr;
  check(fstx_colsrc_dict(s, 0, &dn, &dl, &db, &e), e);
  ArrayHold* h = (ArrayHold*)out->private_data;
  h->dict = new_child();
  out->dictionary = h->dict;
  make_utf8(h->dict, dn, dl, db);
}

#include "arrow_rows.h"  // rows_batch(): the row-list batches (filter matches, lists of rows)

// Runs fn(i, w, threads) for i in [0, n): one task on this thread with all `threads` for its own decoding,
// more on the engine's worker pool (one thread each). The first exception a task throws is rethrown here.
template <typename Fn>
void run_tasks(size_t n, int threads, Fn& fn, fstx_scratchset* ss = nullptr) {
  if (n <= 1) {
    for (size_t i = 0; i < n; ++i) fn(i, (fstx_scratch*)nullptr, threads);
    return;
  }
  struct Ctx {
    Fn* fn;
    std::mutex m;
    std::exception_ptr ex;
  };
  Ctx ctx;
  ctx.fn = &fn;
  fstx_task_fn cb = [](void* p, int64_t i, fstx_scratch* w) -> int {
    Ctx& x = *(Ctx*)p;
    try {
      (*x.fn)((size_t)i, w, 1);
      return 0;
    } catch (...) {
      std::lock_guard<std::mutex> g(x.m);
      if (!x.ex) x.ex = std::current_exception();
      return 1;
    }
  };
  fstx_error e;
  const int rc = fstx_run_tasks((int64_t)n, threads, cb, &ctx, ss, &e);
  if (ctx.ex) std::rethrow_exception(ctx.ex);
  check(rc, e);
}

// A plain string column (utf8 / large_utf8) of a batch over file rows [a, b): its pages decode on the pool,
// one task per page (str_decode, which also counts each page's bytes); a prefix sum over the pages places them
// (str_prep); then every page writes its offsets, bytes and validity straight into the Arrow buffers, again one
// task per page (str_write). Bitmap bytes shared by two pages are put together at the end (str_bits).
struct StrPage {
  const int32_t* lens = nullptr;  // per row: length (-1 = NA), or the dictionary code on a dictionary page
  const char* bytes = nullptr;
  int32_t dict = -1;
  uint64_t first = 0;  // first output row
  uint32_t n = 0;
  uint32_t nulls = 0;
  uint64_t tot = 0;            // bytes of text
  uint64_t off = 0;            // where they start in the text buffer
  uint8_t head = 0, tail = 0;  // validity bits of the first / last bitmap byte when another page shares it
  bool bad = false;            // its text is not valid UTF-8 (checked only when asked)
};

struct StrCol {
  const ColSpec* c = nullptr;
  ArrowArray* out = nullptr;
  uint64_t n = 0;
  SrcPtr s;
  std::vector<DictTab> dicts;
  std::vector<StrPage> pages;
  void* offs = nullptr;
  char* data = nullptr;
  uint8_t* bits = nullptr;
};

void str_open(const Stream& S, StrCol& C, size_t j, uint64_t a, uint64_t b) {
  const bool check = S.utf8_bad != nullptr;
  C.s = open_src(S, *C.c, a, b);
  if (j < S.bufs.size() && S.bufs[j]) fstx_colsrc_set_bufpool(C.s.get(), S.bufs[j]);
  fstx_colsrc_info si;
  fstx_colsrc_info_get(C.s.get(), &si);
  for (uint32_t d = 0; d < si.n_dicts; ++d) {  // dictionaries first (small): pages that use them may then decode
    DictTab t = dict_tab(C.s.get(), d);
    t.bad = check && !valid_utf8((const uint8_t*)t.bytes, (size_t)t.tot);
    C.dicts.push_back(std::move(t));
  }
  C.pages.resize(si.n_pages);
}

void str_decode(StrCol& C, uint32_t q, fstx_scratch* w) {
  StrPage& P = C.pages[q];
  fstx_error e;
  check(fstx_colsrc_load_page(C.s.get(), q, w, &P.lens, &P.bytes, &P.dict, &e), e);
  fstx_colsrc_page_span(C.s.get(), q, &P.first, &P.n);
  uint64_t tot = 0;
  uint32_t nulls = 0;
  if (P.dict >= 0) {
    const DictTab& D = C.dicts[(size_t)P.dict];
    for (uint32_t i = 0; i < P.n; ++i) {
      const int32_t k = P.lens[i];  // checked against the dictionary size by the decoder
      if (k < 0) ++nulls;
      else tot += (uint64_t)std::max(0, D.lens[k]);
    }
  } else {
    for (uint32_t i = 0; i < P.n; ++i) {
      const int32_t l = P.lens[i];
      if (l < 0) ++nulls;
      else tot += (uint64_t)l;
    }
  }
  P.tot = tot;
  P.nulls = nulls;
}

template <typename O>
void str_prep(StrCol& C) {
  uint64_t tot = 0, nulls = 0;
  for (StrPage& P : C.pages) {
    P.off = tot;
    tot += P.tot;
    nulls += P.nulls;
  }
  if (tot > (uint64_t)std::numeric_limits<O>::max())
    throw Fail("column '" + C.c->name + "': a batch holds more than 2 GB of text; use a smaller batch_rows");
  array_init(C.out, (int64_t)C.n, 3);
  C.offs = array_buf(C.out, 1, (size_t)(C.n + 1) * sizeof(O));
  ((O*)C.offs)[C.n] = (O)tot;
  C.data = (char*)array_buf(C.out, 2, (size_t)tot);
  if (nulls) C.bits = (uint8_t*)array_buf(C.out, 0, (size_t)((C.n + 7) / 8));
  C.out->null_count = (int64_t)nulls;
}

inline bool full_byte(uint64_t B, uint64_t r0, uint64_t r1) { return B * 8 >= r0 && B * 8 + 8 <= r1; }

void str_page_bits(StrCol& C, StrPage& P);

template <typename O>
void str_write(StrCol& C, uint32_t q, bool check) {
  StrPage& P = C.pages[q];
  O* o = (O*)C.offs + P.first;
  uint64_t pos = P.off;
  if (P.dict < 0) {  // the page's strings follow each other: one copy
    for (uint32_t i = 0; i < P.n; ++i) {
      o[i] = (O)pos;
      if (P.lens[i] > 0) pos += (uint64_t)P.lens[i];
    }
    if (P.tot) std::memcpy(C.data + P.off, P.bytes, (size_t)P.tot);
    P.bad = check && !valid_utf8((const uint8_t*)C.data + P.off, (size_t)P.tot);  // while the text is in cache
  } else {  // its dictionary was checked in str_open
    const DictTab& D = C.dicts[(size_t)P.dict];
    for (uint32_t i = 0; i < P.n; ++i) {
      o[i] = (O)pos;
      const int32_t k = P.lens[i];
      if (k < 0 || D.lens[k] <= 0) continue;
      std::memcpy(C.data + pos, D.bytes + D.off[(size_t)k], (size_t)D.lens[k]);
      pos += (uint64_t)D.lens[k];
    }
  }
  if (C.bits && P.n) str_page_bits(C, P);
  fstx_colsrc_drop_page(C.s.get(), q);  // the page is in the Arrow buffers now: free it here, in parallel
  P.lens = nullptr;
  P.bytes = nullptr;
}

void str_page_bits(StrCol& C, StrPage& P) {
  const uint64_t r0 = P.first, r1 = P.first + P.n;
  for (uint64_t B = r0 >> 3; B <= (r1 - 1) >> 3; ++B) {
    uint8_t v = 0;
    const uint64_t lo = std::max(r0, B * 8), hi = std::min(r1, B * 8 + 8);
    for (uint64_t r = lo; r < hi; ++r)
      if (P.lens[r - r0] >= 0) v |= (uint8_t)(1u << (r & 7));
    if (full_byte(B, r0, r1)) C.bits[B] = v;
    else if (B == r0 >> 3) P.head = v;
    else P.tail = v;
  }
}

void str_bits(StrCol& C) {
  if (!C.bits) return;
  for (const StrPage& P : C.pages) {  // bytes no page owns alone: clear, then every page adds its bits
    if (!P.n) continue;
    const uint64_t r0 = P.first, r1 = P.first + P.n, h = r0 >> 3, t = (r1 - 1) >> 3;
    if (!full_byte(h, r0, r1)) C.bits[h] = 0;
    if (!full_byte(t, r0, r1)) C.bits[t] = 0;
  }
  for (const StrPage& P : C.pages) {
    if (!P.n) continue;
    const uint64_t r0 = P.first, r1 = P.first + P.n, h = r0 >> 3, t = (r1 - 1) >> 3;
    if (!full_byte(h, r0, r1)) C.bits[h] |= P.head;
    if (t != h && !full_byte(t, r0, r1)) C.bits[t] |= P.tail;
  }
}

// A batch over file rows [a, b) (no row list). Phase 1: string pages decode and fixed-width columns decode
// straight into their Arrow arrays (in row parts when there are fewer columns than threads), all as one set of
// tasks on the pool. Phase 2: string columns are laid out page by page (dictionary output: one task a column).
void batch_range(const Stream& S, uint64_t a, uint64_t b, uint64_t n, ArrayHold* h, std::vector<uint8_t>& bad) {
  const size_t nc = S.cols.size();
  const int parts = nc && nc < (size_t)S.threads ? (int)(((size_t)S.threads + nc - 1) / nc) : 1;
  std::vector<std::unique_ptr<FixedCol>> fx(nc);
  std::vector<std::unique_ptr<StrCol>> sx(nc);
  struct Task {
    size_t j;
    uint64_t i0, i1;  // fixed: output rows [i0, i1); strings: page i0 (phase 2: i1 = 1 for a dictionary column)
  };
  std::vector<Task> tasks;
  for (size_t j = 0; j < nc; ++j) {
    const ColSpec& c = S.cols[j];
    if (c.phys == FSTX_STRING) {
      sx[j].reset(new StrCol());
      StrCol& C = *sx[j];
      C.c = &c;
      C.out = h->kids[j];
      C.n = n;
      str_open(S, C, j, a, b);
      for (uint32_t q = 0; q < (uint32_t)C.pages.size(); ++q) tasks.push_back({j, q, 0});
      continue;
    }
    fx[j].reset(new FixedCol());
    FixedCol& F = *fx[j];
    F.c = &c;
    F.out = h->kids[j];
    F.a = a;
    F.n = n;
    const std::vector<uint64_t> cut = fixed_prep(S, F, b, parts);
    for (size_t q = 0; q + 1 < cut.size(); ++q) tasks.push_back({j, cut[q], cut[q + 1]});
  }
  auto decode = [&](size_t i, fstx_scratch* w, int threads) {
    const Task& t = tasks[i];
    if (fx[t.j]) fixed_part(S, *fx[t.j], t.i0, t.i1, threads, w);
    else str_decode(*sx[t.j], (uint32_t)t.i0, w);
  };
  run_tasks(tasks.size(), S.threads, decode, S.scratch);
  tasks.clear();
  for (size_t j = 0; j < nc; ++j) {
    if (!sx[j]) continue;
    StrCol& C = *sx[j];
    if (C.c->out == O_DICT_STR) {
      tasks.push_back({j, 0, 1});
      continue;
    }
    if (C.c->out == O_UTF8) str_prep<int32_t>(C);
    else str_prep<int64_t>(C);
    if (b >= S.to) fstx_colsrc_set_bufpool(C.s.get(), nullptr);  // last batch: its pages are freed as they go
    for (uint32_t q = 0; q < (uint32_t)C.pages.size(); ++q) tasks.push_back({j, q, 0});
  }
  const bool check = S.utf8_bad != nullptr;
  auto layout = [&](size_t i, fstx_scratch*, int) {
    const Task& t = tasks[i];
    StrCol& C = *sx[t.j];
    if (t.i1) dict_column(*C.c, C.s.get(), n, C.out);  // its dictionary was checked in str_open
    else if (C.c->out == O_UTF8) str_write<int32_t>(C, (uint32_t)t.i0, check);
    else str_write<int64_t>(C, (uint32_t)t.i0, check);
  };
  run_tasks(tasks.size(), S.threads, layout);
  for (size_t j = 0; j < nc; ++j) {
    if (!sx[j]) {
      h->kids[j]->null_count = fx[j]->nulls.load();
      continue;
    }
    str_bits(*sx[j]);
    for (const DictTab& D : sx[j]->dicts) bad[j] |= D.bad ? 1 : 0;
    for (const StrPage& P : sx[j]->pages) bad[j] |= P.bad ? 1 : 0;
  }
}

// The text of a string array (plain, or its dictionary) is valid UTF-8.
bool array_utf8_ok(const ArrowArray* x, int out) {
  if (out == O_DICT_STR) return array_utf8_ok(x->dictionary, O_UTF8);
  const uint64_t n = (uint64_t)x->length;
  const uint64_t end = out == O_LARGE_UTF8 ? (uint64_t)((const int64_t*)x->buffers[1])[n]
                                           : (uint64_t)((const int32_t*)x->buffers[1])[n];
  return valid_utf8((const uint8_t*)x->buffers[2], (size_t)end);
}

Stream* stream_of(ArrowArrayStream* st) { return (Stream*)st->private_data; }

int fail_code(Stream& S, const char* what) {
  S.error = what;
  return EIO;
}

int stream_get_schema(ArrowArrayStream* st, ArrowSchema* out) {
  Stream& S = *stream_of(st);
  ArrowSchema res;
  res.release = nullptr;
  try {
    SchemaHold* h = schema_init(&res, "+s", "", 0);
    for (size_t j = 0; j < S.cols.size(); ++j) {
      h->kids.push_back(new ArrowSchema());
      h->kids.back()->release = nullptr;
    }
    res.n_children = (int64_t)h->kids.size();
    res.children = h->kids.empty() ? nullptr : h->kids.data();
    for (size_t j = 0; j < S.cols.size(); ++j) {
      const ColSpec& c = S.cols[j];
      SchemaHold* k = schema_init(h->kids[j], schema_format(c), c.name, ARROW_FLAG_NULLABLE);
      if (c.out == O_DICT_STR || c.out == O_FACTOR) {
        if (c.ordered) h->kids[j]->flags |= ARROW_FLAG_DICTIONARY_ORDERED;
        k->dict = new ArrowSchema();
        k->dict->release = nullptr;
        schema_init(k->dict, "u", "", 0);
        h->kids[j]->dictionary = k->dict;
      }
    }
    *out = res;
    return 0;
  } catch (const std::bad_alloc&) {
    if (res.release) res.release(&res);
    S.error = "out of memory";
    return ENOMEM;
  } catch (const std::exception& x) {
    if (res.release) res.release(&res);
    return fail_code(S, x.what());
  }
}

int stream_get_next(ArrowArrayStream* st, ArrowArray* out) {
  Stream& S = *stream_of(st);
  ArrowArray res;
  res.release = nullptr;
  try {
    std::lock_guard<std::mutex> g(g_engine);
    uint64_t a, b, n, next;
    std::vector<uint64_t> sel;
    const uint64_t cap = S.batch_rows ? S.batch_rows : UINT64_MAX;
    if (!S.by_rows) {
      if (S.pos >= S.to) {
        std::memset(out, 0, sizeof(*out));  // end of stream
        return 0;
      }
      a = S.pos;
      b = std::min(S.to, rg_end_of(S, a));
      if (b - a > cap) b = a + cap;
      n = b - a;
      next = b;
    } else {
      if (S.pos >= S.rows.size()) {
        std::memset(out, 0, sizeof(*out));
        return 0;
      }
      const uint64_t i0 = S.pos;
      a = S.rows[i0];
      const uint64_t e = rg_end_of(S, a);
      uint64_t i1 = (uint64_t)(std::lower_bound(S.rows.begin() + (ptrdiff_t)i0, S.rows.end(), e) - S.rows.begin());
      if (i1 - i0 > cap) i1 = i0 + cap;
      b = S.rows[i1 - 1] + 1;
      n = i1 - i0;
      sel.resize((size_t)n);
      for (uint64_t i = 0; i < n; ++i) sel[(size_t)i] = S.rows[i0 + i] - a;
      next = i1;
    }
    ArrayHold* h = array_init(&res, (int64_t)n, 1);
    for (size_t j = 0; j < S.cols.size(); ++j) h->kids.push_back(new_child());
    res.n_children = (int64_t)h->kids.size();
    res.children = h->kids.empty() ? nullptr : h->kids.data();
    std::vector<uint8_t> bad(S.cols.size(), 0);
    if (S.by_rows) {
      rows_batch(S, h, a, sel.data(), n);
      if (S.utf8_bad)  // the row-list path lays out its strings on its own: check them here
        for (size_t j = 0; j < S.cols.size(); ++j)
          if (S.cols[j].phys == FSTX_STRING && !array_utf8_ok(h->kids[j], S.cols[j].out)) bad[j] = 1;
    } else {
      batch_range(S, a, b, n, h, bad);
    }
    if (S.utf8_bad && !bad.empty()) std::memcpy(S.utf8_bad, bad.data(), bad.size());
    S.pos = next;
    *out = res;
    return 0;
  } catch (const std::bad_alloc&) {
    if (res.release) res.release(&res);
    S.error = "out of memory";
    return ENOMEM;
  } catch (const std::exception& x) {
    if (res.release) res.release(&res);
    return fail_code(S, x.what());
  }
}

const char* stream_last_error(ArrowArrayStream* st) {
  Stream* S = stream_of(st);
  return S->error.empty() ? nullptr : S->error.c_str();
}

void stream_release(ArrowArrayStream* st) {
  delete stream_of(st);
  st->release = nullptr;
}

}  // namespace

extern "C" int fstx_arrow_stream_open(const char* path, const fstx_arrow_args* A, ArrowArrayStream* out,
                                      fstx_error* err) {
  std::memset(err, 0, sizeof(*err));
  err->column = -1;
  std::unique_ptr<Stream> S;
  try {
    S.reset(new Stream());
    if (fstx_open(path, &S->f, err) != FSTX_OK) {
      S->f = nullptr;
      return err->code;
    }
    fstx_file_info info;
    fstx_file_info_get(S->f, &info);
    auto arg_fail = [&](const std::string& m) {
      err->code = FSTX_ERR_ARG;
      std::snprintf(err->message, sizeof(err->message), "%s", m.c_str());
      return FSTX_ERR_ARG;
    };
    S->by_rows = A->rows != nullptr;
    if (S->by_rows) {
      S->rows.assign(A->rows, A->rows + A->n_rows);
      for (size_t i = 0; i < S->rows.size(); ++i)
        if (S->rows[i] >= info.n_rows || (i && S->rows[i] < S->rows[i - 1]))
          return arg_fail("rows must be ascending and inside the table");
    } else {
      if (A->from > A->to || A->to > info.n_rows) return arg_fail("row range is outside the table");
      S->from = S->pos = A->from;
      S->to = A->to;
    }
    // at most one thread per hardware thread: a batch starts up to this many (threads = 5000 must not mean 5000)
    const int hw = (int)std::max(1u, std::thread::hardware_concurrency());
    S->threads = A->threads > 0 && A->threads < hw ? A->threads : hw;
    S->batch_rows = A->batch_rows;
    S->verify = A->verify;
    S->scratch = fstx_scratchset_new();
    S->plain = (A->flags & FSTX_ARROW_PLAIN_STRINGS) != 0;
    if (A->flags & FSTX_ARROW_CHECK_UTF8) {
      if (!A->utf8_bad) return arg_fail("FSTX_ARROW_CHECK_UTF8 needs utf8_bad");
      S->utf8_bad = A->utf8_bad;
      std::memset(S->utf8_bad, 0, A->n_cols);
    }
    // row-group ends from the page directory (row groups may differ in size after appends)
    const int64_t np = fstx_pages_get(S->f, nullptr, 0);
    std::vector<fstx_page_info> pi((size_t)std::max<int64_t>(np, 0));
    if (np > 0) fstx_pages_get(S->f, pi.data(), (uint64_t)np);
    std::vector<uint64_t> ends(info.n_row_groups, 0);
    for (const fstx_page_info& p : pi)
      if (!p.is_dictionary && p.row_group < ends.size())
        ends[p.row_group] = std::max(ends[p.row_group], p.first_row + p.row_count);
    std::sort(ends.begin(), ends.end());
    ends.erase(std::unique(ends.begin(), ends.end()), ends.end());
    if (ends.empty() || ends.back() < info.n_rows) ends.push_back(info.n_rows);
    S->rg_ends = ends;
    const uint64_t r0 = S->by_rows ? (S->rows.empty() ? 0 : S->rows.front()) : S->from;
    const uint64_t r1 = S->by_rows ? (S->rows.empty() ? 0 : S->rows.back() + 1) : S->to;
    for (uint32_t j = 0; j < A->n_cols; ++j) {
      if (A->cols[j] >= info.n_cols) return arg_fail("column index out of range");
      fstx_column_info ci;
      if (fstx_column_info_get(S->f, A->cols[j], &ci) != FSTX_OK) return arg_fail("column index out of range");
      ColSpec c;
      c.col = A->cols[j];
      c.name = A->names[j];
      c.phys = ci.phys;
      c.ordered = ci.ordered != 0;
      if (ci.logical == FSTX_LT_FACTOR) {
        c.out = O_FACTOR;
        for (uint32_t i = 0; i < ci.n_levels; ++i) {
          const char* s = nullptr;
          uint32_t len = 0;
          if (fstx_level_get(S->f, c.col, i, &s, &len) != FSTX_OK || len > (uint32_t)INT32_MAX ||
              !valid_utf8((const uint8_t*)s, len)) {
            err->code = FSTX_ERR_CORRUPT;
            err->column = c.col;
            std::snprintf(err->message, sizeof(err->message), "column '%s': factor level %u is not valid UTF-8",
                          c.name.c_str(), i + 1);
            return FSTX_ERR_CORRUPT;
          }
          c.lev_lens.push_back((int32_t)len);
          c.lev_bytes.append(s, len);
        }
      } else if (ci.logical == FSTX_LT_DATE && !A->raw) {
        c.out = O_DATE;
      } else if (ci.logical == FSTX_LT_POSIXCT && !A->raw) {
        c.out = O_TS;
        c.has_tz = ci.has_tzone != 0;
        if (c.has_tz) c.tz.assign(ci.tzone, ci.tzone_len);
      } else if (ci.phys == FSTX_INT32) {
        c.out = O_INT32;
      } else if (ci.phys == FSTX_FLOAT64) {
        c.out = O_FLOAT64;
      } else if (ci.phys == FSTX_LOGICAL) {
        c.out = O_BOOL;
      } else {
        // dictionary when every page in the rows read is dictionary-coded (the schema never changes between
        // batches) and the caller did not ask for plain strings; large_utf8 when the column's decoded text could
        // pass 2 GB in one batch (FSST expands at most 8x; shortcut: bounded by the whole column, so a big file can
        // get large_utf8 with small batches)
        c.out = O_UTF8;
        if (r1 > r0 && !S->plain) {
          fstx_colsrc* s = nullptr;
          if (fstx_colsrc_open(S->f, c.col, r0, r1, 0, 0, &s, err) != FSTX_OK) return err->code;
          fstx_colsrc_info si;
          fstx_colsrc_info_get(s, &si);
          fstx_colsrc_free(s);
          if (si.all_dict) c.out = O_DICT_STR;
        }
        if (c.out == O_UTF8 && ci.raw_bytes > (uint64_t)INT32_MAX / 8) c.out = O_LARGE_UTF8;
      }
      S->bufs.push_back(c.phys == FSTX_STRING ? fstx_bufpool_new() : nullptr);
      S->cols.push_back(std::move(c));
    }
  } catch (const std::bad_alloc&) {
    err->code = FSTX_ERR_MEMORY;
    std::snprintf(err->message, sizeof(err->message), "out of memory");
    return FSTX_ERR_MEMORY;
  }
  std::memset(out, 0, sizeof(*out));
  out->get_schema = stream_get_schema;
  out->get_next = stream_get_next;
  out->get_last_error = stream_last_error;
  out->release = stream_release;
  out->private_data = S.release();
  return FSTX_OK;
}

// ---------------------------------------------------------------- R glue
#ifndef FSTX_ARROW_NO_R
#include <cpp11.hpp>

namespace {

SEXP arg(SEXP l, const char* nm) {
  SEXP names = Rf_getAttrib(l, R_NamesSymbol);
  if (TYPEOF(l) != VECSXP || TYPEOF(names) != STRSXP) return R_NilValue;
  for (R_xlen_t i = 0; i < Rf_xlength(l); ++i)
    if (std::strcmp(CHAR(STRING_ELT(names, i)), nm) == 0) return VECTOR_ELT(l, i);
  return R_NilValue;
}

ArrowArrayStream* stream_xp(SEXP xp) {
  if (TYPEOF(xp) != EXTPTRSXP || !Rf_inherits(xp, "nanoarrow_array_stream"))
    cpp11::stop("fstx: internal error, not a nanoarrow_array_stream");
  ArrowArrayStream* st = (ArrowArrayStream*)R_ExternalPtrAddr(xp);
  if (!st) cpp11::stop("fstx: internal error, empty stream pointer");
  return st;
}

}  // namespace

// args$op: NULL (open a stream into args$stream), "live" (live holder count, tests), "pull_thread" (tests: pull
// every batch of args$stream on a new thread that is not R's, return batches, rows and any error).
[[cpp11::register]]
SEXP fstx_arrow_stream_impl(SEXP args) {
  SEXP op = arg(args, "op");
  if (op != R_NilValue && (TYPEOF(op) != STRSXP || Rf_xlength(op) != 1)) cpp11::stop("fstx: internal error, bad op");
  const std::string o = op == R_NilValue ? "" : CHAR(STRING_ELT(op, 0));
  using namespace cpp11::literals;
  if (o == "live") return cpp11::as_sexp((double)g_live.load());
  if (o == "pull_thread") {
    ArrowArrayStream* st = stream_xp(arg(args, "stream"));
    if (!st->release) cpp11::stop("fstx: the stream was already released");
    double batches = 0, rows = 0;
    std::string error;
    bool other_thread = false;
    const std::thread::id me = std::this_thread::get_id();
    std::thread t([&] {
      other_thread = std::this_thread::get_id() != me;
      ArrowSchema sc;
      if (st->get_schema(st, &sc) != 0) {
        const char* m = st->get_last_error(st);
        error = m ? m : "get_schema failed";
        return;
      }
      sc.release(&sc);
      for (;;) {
        ArrowArray a;
        if (st->get_next(st, &a) != 0) {
          const char* m = st->get_last_error(st);
          error = m ? m : "get_next failed";
          return;
        }
        if (!a.release) return;
        batches += 1;
        rows += (double)a.length;
        a.release(&a);
      }
    });
    t.join();
    return cpp11::writable::list({"batches"_nm = cpp11::as_sexp(batches), "rows"_nm = cpp11::as_sexp(rows),
                                  "error"_nm = cpp11::as_sexp(error.c_str()),
                                  "other_thread"_nm = cpp11::as_sexp(other_thread)});
  }
  ArrowArrayStream* out = stream_xp(arg(args, "stream"));
  if (out->release) cpp11::stop("fstx: internal error, the stream is not empty");
  SEXP path = arg(args, "path");
  SEXP cols = arg(args, "cols"), names = arg(args, "names"), rows = arg(args, "rows");
  if (TYPEOF(path) != STRSXP || Rf_xlength(path) != 1 || TYPEOF(cols) != INTSXP || TYPEOF(names) != STRSXP ||
      Rf_xlength(names) != Rf_xlength(cols) || (rows != R_NilValue && TYPEOF(rows) != REALSXP))
    cpp11::stop("fstx: internal error, bad arguments to the Arrow stream");
  // C++ objects are alive from here on: R calls that can raise an error go through cpp11::safe
  const std::string p = cpp11::safe[Rf_translateCharUTF8](STRING_ELT(path, 0));
  std::vector<uint32_t> ci;
  std::vector<std::string> nm;
  std::vector<const char*> np;
  const int* cp = cpp11::safe[INTEGER_RO](cols);
  for (R_xlen_t j = 0; j < Rf_xlength(cols); ++j) {
    ci.push_back((uint32_t)cp[j]);
    nm.push_back(cpp11::safe[Rf_translateCharUTF8](STRING_ELT(names, j)));
  }
  for (const std::string& s : nm) np.push_back(s.c_str());
  std::vector<uint64_t> rv;
  if (rows != R_NilValue) {
    const double* r = cpp11::safe[REAL_RO](rows);
    for (R_xlen_t i = 0; i < Rf_xlength(rows); ++i) rv.push_back((uint64_t)r[i] - 1);  // checked in R: whole, >= 1
  }
  fstx_arrow_args A;
  std::memset(&A, 0, sizeof(A));
  A.cols = ci.data();
  A.names = np.data();
  A.n_cols = (uint32_t)ci.size();
  A.from = (uint64_t)cpp11::safe[Rf_asReal](arg(args, "from"));
  A.to = (uint64_t)cpp11::safe[Rf_asReal](arg(args, "to"));
  A.rows = rows == R_NilValue ? nullptr : rv.data();
  A.n_rows = rv.size();
  static const uint64_t no_row = 0;
  if (rows != R_NilValue && rv.empty()) A.rows = &no_row;  // an empty selection: a stream with no batches
  A.threads = cpp11::safe[Rf_asInteger](arg(args, "threads"));
  SEXP br = arg(args, "batch_rows");
  A.batch_rows = br == R_NilValue ? 0 : (uint64_t)cpp11::safe[Rf_asReal](br);
  SEXP vf = arg(args, "verify");  // FSTX_VERIFY_* bits from R (.verify_bits())
  A.verify = vf == R_NilValue ? FSTX_VERIFY_HASH : cpp11::safe[Rf_asInteger](vf);
  fstx_error err;
  if (fstx_arrow_stream_open(p.c_str(), &A, out, &err) != FSTX_OK) cpp11::stop("fstx: %s", err.message);
  return R_NilValue;
}
#endif
