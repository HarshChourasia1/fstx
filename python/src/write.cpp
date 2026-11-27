// fstx Python binding: writing. MIT licence.
// fstx/_write.py decides every type question with pyarrow and hands over, per column, the stored definition and
// Arrow arrays: the chunks of a number column as they are (int8 / int16 / int32 / uint8 / uint16 / int64 / bool /
// date32 / float32 / float64, nulls still nulls), or string / binary chunks, plain or dictionary-coded (int32 codes
// into one dictionary shared by all chunks, so that equal strings reach the engine as one pointer, as R's string
// cache gives them; the engine's string dictionary keys on pointers). This file reads their buffers through the
// Arrow C data interface and runs the engine with the GIL released. The engine takes one int32 / double pointer
// per number column, so rows go to it a window of whole row groups at a time (the file is the same as from one
// call): a number column whose window lies in one chunk that is already what the engine takes is passed in place,
// any other is filled (widened, NA written for nulls) into a window buffer, in parallel. The string callback runs
// on engine worker threads and touches nothing but the Arrow buffers.
#include "binding.h"

#include <nanobind/stl/string.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "arrow_abi.h"

namespace fstxpy {
namespace {

struct StrChunk {
  const uint8_t* valid = nullptr;  // validity bitmap or null (no missing values)
  const void* offsets = nullptr;   // int32 or int64, already moved to the chunk's first row
  const char* bytes = nullptr;
  int64_t bit0 = 0;                // bit of row 0 in `valid`
  uint64_t rows = 0;
  bool large = false;              // 64-bit offsets
  bool text = true;                // false: binary handed back as it was read (bytes are not checked as UTF-8)
  // Dictionary-coded chunk: `rows` int32 codes at `idx` (moved to the chunk's first row; validity `ivalid` from
  // bit `ibit0`). valid / offsets / bytes / bit0 / large / text then describe the dictionary of `dict_rows` values.
  const int32_t* idx = nullptr;
  const uint8_t* ivalid = nullptr;
  int64_t ibit0 = 0;
  uint64_t dict_rows = 0;
};

struct StrSrc {
  std::vector<StrChunk> chunks;
  std::vector<uint64_t> first;  // first row of each chunk
  uint64_t base = 0;            // row of the table where the current engine call starts
};

inline bool bit_off(const uint8_t* bits, int64_t b) { return bits && !((bits[b >> 3] >> (b & 7)) & 1); }

// ---- number columns
enum NumKind : uint8_t { N_I8, N_I16, N_I32, N_U8, N_U16, N_I64, N_BOOL, N_F32, N_F64 };

struct NumChunk {
  NumKind kind = N_I32;
  const void* values = nullptr;    // the values buffer (bits for bool), at element 0 of the buffer
  const uint8_t* valid = nullptr;  // validity bitmap or null (no missing values)
  int64_t off = 0;                 // the chunk's first element in both buffers
  uint64_t rows = 0;
};

struct NumSrc {
  std::vector<NumChunk> chunks;
  std::vector<uint64_t> first;  // first row of each chunk
  int32_t add = 0;              // added to every value (int32 targets)
  bool bits = false;            // int64 kept bit for bit in a double (integer64), NA = INT64_MIN
};

const int32_t kNaInt = INT32_MIN;
const uint64_t kNaRealBits = 0x7FF00000000007A2ull;  // R's NA_real_
const uint64_t kRowGroup = 1048576;                 // the engine's default row group, the one files are written with

// The Arrow formats a number column is taken in, by target (the binding converts them itself).
bool num_kind(const std::string& f, int phys, NumKind* k) {
  static const struct {
    const char* fmt;
    NumKind kind;
    bool to_int, to_dbl;
  } tab[] = {{"c", N_I8, true, false},   {"s", N_I16, true, false},  {"i", N_I32, true, true},
             {"C", N_U8, true, false},   {"S", N_U16, true, false},  {"l", N_I64, true, true},
             {"b", N_BOOL, true, false}, {"tdD", N_I32, true, true}, {"f", N_F32, false, true},
             {"g", N_F64, false, true}};
  for (const auto& t : tab)
    if (f == t.fmt && (phys == FSTX_FLOAT64 ? t.to_dbl : t.to_int)) {
      *k = t.kind;
      return true;
    }
  return false;
}

template <typename T>
inline void widen_i32(const T* x, uint64_t n, int32_t add, int32_t* out) {
  for (uint64_t i = 0; i < n; ++i) out[i] = (int32_t)x[i] + add;
}
template <typename T>
inline void to_dbl(const T* x, uint64_t n, double* out) {
  for (uint64_t i = 0; i < n; ++i) out[i] = (double)x[i];
}

// Rows [i0, i0 + n) of one chunk into out (int32 or double, by phys), NA where the chunk has a null.
void fill_chunk(const NumChunk& c, uint64_t i0, uint64_t n, int phys, int32_t add, bool bits, void* out) {
  const int64_t e0 = c.off + (int64_t)i0;  // element of the values buffer
  if (phys != FSTX_FLOAT64) {
    int32_t* o = (int32_t*)out;
    switch (c.kind) {
      case N_I8: widen_i32((const int8_t*)c.values + e0, n, add, o); break;
      case N_I16: widen_i32((const int16_t*)c.values + e0, n, add, o); break;
      case N_U8: widen_i32((const uint8_t*)c.values + e0, n, add, o); break;
      case N_U16: widen_i32((const uint16_t*)c.values + e0, n, add, o); break;
      case N_I64: widen_i32((const int64_t*)c.values + e0, n, add, o); break;  // _write.py checked the range
      case N_I32:
        if (add) widen_i32((const int32_t*)c.values + e0, n, add, o);
        else std::memcpy(o, (const int32_t*)c.values + e0, n * 4);
        break;
      case N_BOOL: {
        const uint8_t* b = (const uint8_t*)c.values;
        for (uint64_t i = 0; i < n; ++i) {
          const int64_t k = e0 + (int64_t)i;
          o[i] = (b[k >> 3] >> (k & 7)) & 1;
        }
        break;
      }
      default: break;
    }
    if (c.valid)
      for (uint64_t i = 0; i < n; ++i)
        if (bit_off(c.valid, e0 + (int64_t)i)) o[i] = kNaInt;
    return;
  }
  double* o = (double*)out;
  switch (c.kind) {
    case N_F64: std::memcpy(o, (const double*)c.values + e0, n * 8); break;
    case N_F32: to_dbl((const float*)c.values + e0, n, o); break;
    case N_I32: to_dbl((const int32_t*)c.values + e0, n, o); break;
    case N_I64:
      if (bits) std::memcpy(o, (const int64_t*)c.values + e0, n * 8);
      else to_dbl((const int64_t*)c.values + e0, n, o);
      break;
    default: break;
  }
  if (c.valid) {
    const uint64_t na = bits ? (uint64_t)INT64_MIN : kNaRealBits;
    for (uint64_t i = 0; i < n; ++i)
      if (bit_off(c.valid, e0 + (int64_t)i)) std::memcpy(o + i, &na, 8);
  }
}

// Rows [r0, r0 + n) of a column into out.
void fill_rows(const NumSrc& s, int phys, uint64_t r0, uint64_t n, void* out) {
  const size_t w = phys == FSTX_FLOAT64 ? 8 : 4;
  size_t k = (size_t)(std::upper_bound(s.first.begin(), s.first.end(), r0) - s.first.begin()) - 1;
  uint64_t done = 0;
  while (done < n) {
    const NumChunk& c = s.chunks[k];
    const uint64_t i0 = r0 + done - s.first[k];
    const uint64_t m = std::min<uint64_t>(n - done, c.rows - i0);
    fill_chunk(c, i0, m, phys, s.add, s.bits, (char*)out + done * w);
    done += m;
    ++k;
  }
}

// The window's rows in place when one chunk holds them all as the engine takes them, else null.
const void* in_place(const NumSrc& s, int phys, uint64_t r0, uint64_t n) {
  if (s.chunks.empty() || s.add) return nullptr;
  const size_t k = (size_t)(std::upper_bound(s.first.begin(), s.first.end(), r0) - s.first.begin()) - 1;
  const NumChunk& c = s.chunks[k];
  const uint64_t i0 = r0 - s.first[k];
  if (c.valid || i0 + n > c.rows) return nullptr;
  if (phys == FSTX_FLOAT64 ? !(c.kind == N_F64 || (c.kind == N_I64 && s.bits)) : c.kind != N_I32) return nullptr;
  return (const char*)c.values + (size_t)(c.off + (int64_t)i0) * (phys == FSTX_FLOAT64 ? 8 : 4);
}

// Runs job(0 .. n-1) on up to `threads` threads (this one included).
template <typename F>
void run_parallel(size_t n, int threads, F job) {
  const size_t t = std::min<size_t>(n, (size_t)std::max(threads, 1));
  if (t <= 1) {
    for (size_t i = 0; i < n; ++i) job(i);
    return;
  }
  std::atomic<size_t> next(0);
  auto work = [&]() {
    for (size_t i; (i = next.fetch_add(1)) < n;) job(i);
  };
  std::vector<std::thread> pool;
  pool.reserve(t - 1);
  for (size_t i = 1; i < t; ++i) pool.emplace_back(work);
  work();
  for (std::thread& th : pool) th.join();
}
inline bool is_null(const StrChunk& c, uint64_t i) { return bit_off(c.valid, c.bit0 + (int64_t)i); }
inline void span_of(const StrChunk& c, uint64_t i, int64_t* a, int64_t* b) {
  if (c.large) {
    const int64_t* o = (const int64_t*)c.offsets;
    *a = o[i];
    *b = o[i + 1];
  } else {
    const int32_t* o = (const int32_t*)c.offsets;
    *a = o[i];
    *b = o[i + 1];
  }
}

// Engine callback (worker threads): reads Arrow buffers only. Lengths were checked by check_strings.
int get_strings(void* ctx, uint64_t row, uint32_t n, const char** ptrs, int32_t* lens) {
  const StrSrc* s = (const StrSrc*)ctx;
  row += s->base;
  size_t k = (size_t)(std::upper_bound(s->first.begin(), s->first.end(), row) - s->first.begin()) - 1;
  uint64_t i = row - s->first[k];
  for (uint32_t j = 0; j < n; ++j) {
    while (i >= s->chunks[k].rows) {
      ++k;
      i = 0;
    }
    const StrChunk& c = s->chunks[k];
    uint64_t v = i;  // the value's position: the row, or its code in the dictionary
    bool na = false;
    if (c.idx) {
      na = bit_off(c.ivalid, c.ibit0 + (int64_t)i);
      if (!na) v = (uint64_t)c.idx[i];
    }
    if (na || is_null(c, v)) {
      ptrs[j] = nullptr;
      lens[j] = -1;
    } else {
      int64_t a, b;
      span_of(c, v, &a, &b);
      ptrs[j] = c.bytes + a;
      lens[j] = (int32_t)(b - a);
    }
    ++i;
  }
  return 0;
}

// Well-formed UTF-8 (no overlongs, surrogates or values past U+10FFFF) without NUL: 0 ok, 1 not UTF-8, 2 NUL.
int text_problem(const unsigned char* p, int64_t n, bool text) {
  int64_t i = 0;
  while (i < n) {
    const unsigned char c = p[i];
    if (c == 0) return 2;
    if (c < 0x80 || !text) {
      ++i;
      continue;
    }
    int need;
    unsigned lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) need = 1;
    else if (c == 0xE0) need = 2, lo = 0xA0;
    else if (c == 0xED) need = 2, hi = 0x9F;
    else if (c >= 0xE1 && c <= 0xEF) need = 2;
    else if (c == 0xF0) need = 3, lo = 0x90;
    else if (c >= 0xF1 && c <= 0xF3) need = 3;
    else if (c == 0xF4) need = 3, hi = 0x8F;
    else return 1;
    if (i + need >= n) return 1;  // cut short
    if (p[i + 1] < lo || p[i + 1] > hi) return p[i + 1] == 0 ? 2 : 1;
    for (int k = 2; k <= need; ++k)
      if (p[i + k] < 0x80 || p[i + k] > 0xBF) return p[i + k] == 0 ? 2 : 1;
    i += need + 1;
  }
  return 0;
}

// The `n` values of a plain chunk or of a dictionary: sound offsets, at most 2^31-1 bytes each, no NUL, and UTF-8
// for text. Returns 0, or a problem code (1 not UTF-8, 2 NUL, 3 too long, 4 bad offsets) with *at set to the
// first value that shows it. With `codes` (n entries) every value's own code is kept there and checking goes on
// past a bad value; 4 still stops at once.
int check_values(const StrChunk& c, uint64_t n, uint64_t* at, std::vector<uint8_t>* codes) {
  if (n == 0) return 0;
  int64_t a0, b0, a1, b1;
  span_of(c, 0, &a0, &b0);
  span_of(c, n - 1, &a1, &b1);
  bool plain = a0 >= 0 && b1 >= a0;
  if (plain) {  // fast path: the whole byte range is ASCII without NUL, so every string in it is fine
    const unsigned char* p = (const unsigned char*)c.bytes + a0;
    const int64_t len = b1 - a0;
    const uint64_t ones = 0x0101010101010101ull, highs = 0x8080808080808080ull;
    uint64_t any = 0, zero = 0;
    int64_t i = 0;
    for (; i + 8 <= len; i += 8) {  // 8 bytes a step: any high bit, and any zero byte via (w - 1s) & ~w & 80s
      uint64_t w;
      std::memcpy(&w, p + i, 8);
      any |= w;
      zero |= (w - ones) & ~w & highs;
    }
    for (; i < len; ++i) {
      any |= p[i];
      zero |= p[i] == 0;
    }
    plain = !(any & highs) && !zero;
  }
  if (codes) codes->assign((size_t)n, 0);
  int first = 0;
  int64_t prev = a0;
  for (uint64_t i = 0; i < n; ++i) {
    int64_t a, b;
    span_of(c, i, &a, &b);
    if (a < 0 || b < a || a < prev) {
      *at = i;
      return 4;
    }
    prev = b;
    if (plain || is_null(c, i)) continue;
    int r = b - a > 0x7FFFFFFF ? 3 : text_problem((const unsigned char*)c.bytes + a, b - a, c.text);
    if (!r) continue;
    if (!codes) {
      *at = i;
      return r;
    }
    (*codes)[(size_t)i] = (uint8_t)r;
    if (!first) first = r, *at = i;
  }
  return first;
}

// Every string of a column before the engine sees it. Returns 0, or a problem code (as check_values) with *row
// set to the first row that shows it. A bad dictionary value that no row uses is never written, so is let be.
int check_strings(const StrSrc& s, uint64_t* row) {
  std::vector<uint8_t> codes;
  const void* checked = nullptr;  // the dictionary checked last (the chunks normally share one)
  int dr = 0;                     // its result
  for (size_t k = 0; k < s.chunks.size(); ++k) {
    const StrChunk& c = s.chunks[k];
    if (c.rows == 0) continue;
    uint64_t at = 0;
    if (!c.idx) {
      const int r = check_values(c, c.rows, &at, nullptr);
      *row = s.first[k] + at;
      if (r) return r;
      continue;
    }
    if (!checked || checked != c.offsets) {
      dr = check_values(c, c.dict_rows, &at, &codes);
      checked = c.offsets;
    }
    *row = s.first[k];
    if (dr == 4) return 4;
    for (uint64_t i = 0; i < c.rows; ++i) {
      if (bit_off(c.ivalid, c.ibit0 + (int64_t)i)) continue;
      const int32_t v = c.idx[i];
      *row = s.first[k] + i;
      if (v < 0 || (uint64_t)v >= c.dict_rows) return 4;
      if (dr && codes[(size_t)v]) return codes[(size_t)v];
    }
  }
  return 0;
}

const void* capsule(nb::handle h, const char* name) {
  void* p = PyCapsule_GetPointer(h.ptr(), name);
  if (!p) throw nb::python_error();
  return p;
}

struct Col {
  std::string name, tz;
  std::vector<std::string> levels;
  std::vector<const char*> lv_ptr;
  std::vector<uint32_t> lv_len;
  std::string attrs;
  bool has_attrs = false, has_tz = false;
  StrSrc src;
  NumSrc num;
  std::vector<uint8_t> buf;  // the window's rows, when they are filled
};

std::string bytes_of(nb::handle h) {
  char* p = nullptr;
  Py_ssize_t n = 0;
  if (PyBytes_AsStringAndSize(h.ptr(), &p, &n) != 0) throw nb::python_error();
  return std::string(p, (size_t)n);
}

// columns: [(name, phys, logical, ordered, levels | None, tzone | None, attrs | None, chunks, add, bits)], chunks a
// list of the (schema capsule, array capsule) pairs of pyarrow's Array.__arrow_c_array__(); add and bits as
// NumSrc (numbers only). The caller keeps them alive.
void write(const std::string& path, nb::list columns, nb::object table_attrs, uint64_t nrow, int codec, int level,
           int threads, int profile, int keep_pct, bool append) {
  static const char dummy[8] = {0};
  const size_t nc = nb::len(columns);
  std::vector<Col> cols(nc);
  std::vector<fstx_write_column> defs(nc);
  std::vector<fstx_column_data> data(nc);
  for (size_t j = 0; j < nc; ++j) {
    nb::tuple t = nb::cast<nb::tuple>(columns[j]);
    if (nb::len(t) != 10) throw nb::value_error("fstx: internal error, bad column spec");
    Col& c = cols[j];
    fstx_write_column& d = defs[j];
    std::memset(&d, 0, sizeof(d));
    std::memset(&data[j], 0, sizeof(data[j]));
    c.name = nb::cast<std::string>(t[0]);
    d.name = c.name.c_str();
    d.name_len = (uint32_t)c.name.size();
    d.phys = nb::cast<int>(t[1]);
    d.logical = nb::cast<int>(t[2]);
    d.ordered = nb::cast<bool>(t[3]) ? 1 : 0;
    if (!t[4].is_none()) {
      for (nb::handle h : t[4]) c.levels.push_back(nb::cast<std::string>(h));
      for (const std::string& s : c.levels) {
        c.lv_ptr.push_back(s.data());
        c.lv_len.push_back((uint32_t)s.size());
      }
      d.n_levels = (uint32_t)c.levels.size();
      d.levels = c.lv_ptr.data();
      d.level_lens = c.lv_len.data();
    }
    if (!t[5].is_none()) {
      c.tz = nb::cast<std::string>(t[5]);
      c.has_tz = true;
      d.has_tzone = 1;
      d.tzone = c.tz.c_str();
      d.tzone_len = (uint32_t)c.tz.size();
    }
    if (!t[6].is_none()) {
      c.attrs = bytes_of(t[6]);
      d.attrs = (const uint8_t*)c.attrs.data();
      d.attrs_len = c.attrs.size();
    }
    nb::list chunks = nb::cast<nb::list>(t[7]);
    const size_t nk = nb::len(chunks);
    c.num.add = nb::cast<int32_t>(t[8]);
    c.num.bits = nb::cast<bool>(t[9]);
    uint64_t rows = 0;
    for (size_t k = 0; k < nk; ++k) {
      nb::tuple pair = nb::cast<nb::tuple>(chunks[k]);
      if (nb::len(pair) != 2) throw nb::value_error("fstx: internal error, bad column chunk");
      const ArrowSchema* sc = (const ArrowSchema*)capsule(pair[0], "arrow_schema");
      const ArrowArray* ar = (const ArrowArray*)capsule(pair[1], "arrow_array");
      const std::string fmt = sc->format ? sc->format : "";
      if (ar->length < 0 || ar->offset < 0) throw nb::value_error("fstx: internal error, bad Arrow array");
      if (d.phys == FSTX_STRING) {
        StrChunk s;
        s.rows = (uint64_t)ar->length;
        const ArrowSchema* vs = sc;  // the values: the array itself, or its dictionary
        const ArrowArray* va = ar;
        if (fmt == "i" && sc->dictionary && ar->dictionary) {  // int32 codes into a dictionary of strings
          if (ar->n_buffers != 2 || (s.rows && !ar->buffers[1]))
            throw nb::value_error("fstx: internal error, bad Arrow dictionary array");
          s.idx = s.rows ? (const int32_t*)ar->buffers[1] + ar->offset : nullptr;
          s.ivalid = ar->null_count == 0 ? nullptr : (const uint8_t*)ar->buffers[0];
          s.ibit0 = ar->offset;
          vs = sc->dictionary;
          va = ar->dictionary;
          if (va->length < 0 || va->offset < 0) throw nb::value_error("fstx: internal error, bad Arrow array");
          s.dict_rows = (uint64_t)va->length;
        }
        const std::string vfmt = vs->format ? vs->format : "";
        if (vfmt != "u" && vfmt != "U" && vfmt != "z" && vfmt != "Z")
          throw nb::value_error("fstx: internal error, a string column needs string or binary arrays");
        if (va->n_buffers != 3) throw nb::value_error("fstx: internal error, bad Arrow string array");
        s.large = vfmt == "U" || vfmt == "Z";
        s.text = vfmt == "u" || vfmt == "U";
        s.valid = va->null_count == 0 ? nullptr : (const uint8_t*)va->buffers[0];
        s.bit0 = va->offset;
        if (s.rows) {
          if (!s.idx || s.dict_rows) {  // codes into an empty dictionary are all missing, or check_strings stops
            if (!va->buffers[1]) throw nb::value_error("fstx: internal error, Arrow string array without offsets");
            s.offsets = s.large ? (const void*)((const int64_t*)va->buffers[1] + va->offset)
                                : (const void*)((const int32_t*)va->buffers[1] + va->offset);
            s.bytes = va->buffers[2] ? (const char*)va->buffers[2] : dummy;
          }
          c.src.first.push_back(rows);
          c.src.chunks.push_back(s);
        }
      } else {
        NumChunk n;
        if (!num_kind(fmt, d.phys, &n.kind) || ar->n_buffers != 2)
          throw nb::value_error("fstx: internal error, a number column of an Arrow type the binding does not take");
        n.rows = (uint64_t)ar->length;
        n.off = ar->offset;
        n.valid = ar->null_count != 0 && ar->buffers[0] ? (const uint8_t*)ar->buffers[0] : nullptr;
        n.values = ar->buffers[1];
        if (n.rows) {
          if (!n.values) throw nb::value_error("fstx: internal error, Arrow array without values");
          c.num.first.push_back(rows);
          c.num.chunks.push_back(n);
        }
      }
      rows += (uint64_t)ar->length;
    }
    if (rows != nrow) throw nb::value_error("fstx: internal error, a column has the wrong number of rows");
    if (d.phys == FSTX_STRING) {
      data[j].get_strings = get_strings;
      data[j].ctx = &c.src;
    } else {
      data[j].data = dummy;
    }
  }
  std::string ta;
  const bool has_ta = !table_attrs.is_none();
  if (has_ta) ta = bytes_of(table_attrs);

  fstx_write_options opt;
  std::memset(&opt, 0, sizeof(opt));
  opt.codec = codec;
  opt.level = level;
  opt.threads = threads;
  opt.profile = profile;
  opt.keep_pct = keep_pct;
  // whole row groups per engine call: an append keeps the file's row group size, which is not known here, so it
  // goes in one call
  if (!append) opt.row_group_size = kRowGroup;
  const uint64_t win = append ? std::max<uint64_t>(nrow, 1) : kRowGroup;
  const int nthreads = threads > 0 ? threads : (int)fstx_hardware_threads();

  fstx_error err;
  std::memset(&err, 0, sizeof(err));
  int rc = FSTX_OK, bad = 0;
  size_t bad_col = 0;
  uint64_t bad_row = 0;
  {
    nb::gil_scoped_release nogil;  // no Python object is touched until this block ends
    for (size_t j = 0; j < nc && !bad; ++j) {
      if (defs[j].phys != FSTX_STRING) continue;
      bad = check_strings(cols[j].src, &bad_row);
      bad_col = j;
    }
    if (!bad) {
      fstx_writer* w = nullptr;
      const uint8_t* tap = has_ta ? (const uint8_t*)ta.data() : nullptr;
      rc = append ? fstx_writer_open_append(path.c_str(), defs.data(), (uint32_t)nc, tap, ta.size(), &opt, &w, &err)
                  : fstx_writer_open(path.c_str(), defs.data(), (uint32_t)nc, tap, ta.size(), &opt, &w, &err);
      if (rc == FSTX_OK) {
        try {
          for (uint64_t r0 = 0; r0 < nrow && rc == FSTX_OK; r0 += win) {
            const uint64_t n = std::min(win, nrow - r0);
            struct Piece {
              size_t col;
              uint64_t a, b;  // rows of the window
            };
            std::vector<Piece> pieces;
            for (size_t j = 0; j < nc; ++j) {
              Col& c = cols[j];
              if (defs[j].phys == FSTX_STRING) {
                c.src.base = r0;
                continue;
              }
              if (const void* p = in_place(c.num, defs[j].phys, r0, n)) {
                data[j].data = p;
                continue;
              }
              const size_t wd = defs[j].phys == FSTX_FLOAT64 ? 8 : 4;
              if (c.buf.size() < n * wd) c.buf.resize(std::min(win, nrow) * wd);
              data[j].data = c.buf.data();
              for (uint64_t a = 0; a < n; a += 65536) pieces.push_back({j, a, std::min(n, a + 65536)});
            }
            run_parallel(pieces.size(), nthreads, [&](size_t i) {
              const Piece& q = pieces[i];
              const int ph = defs[q.col].phys;
              fill_rows(cols[q.col].num, ph, r0 + q.a, q.b - q.a,
                        cols[q.col].buf.data() + q.a * (ph == FSTX_FLOAT64 ? 8 : 4));
            });
            rc = fstx_writer_append(w, data.data(), n, &err);
          }
        } catch (const std::exception& e) {  // no memory for a window buffer, or no thread
          rc = FSTX_ERR_MEMORY;
          std::snprintf(err.message, sizeof(err.message), "write_fstx: %s", e.what());
          err.code = rc;
        }
        if (rc != FSTX_OK) fstx_writer_abort(w);
        else rc = fstx_writer_close(w, &err);
      }
    }
  }
  if (bad) {
    static const char* why[] = {"", "a string that is not valid UTF-8", "a string with a NUL character",
                                "a string longer than 2 GB", "damaged string offsets"};
    const std::string msg = "write_fstx: column '" + cols[bad_col].name + "' has " + why[bad] + " (row " +
                            std::to_string(bad_row) + "), which fstx cannot store";
    throw nb::value_error(msg.c_str());
  }
  check(rc, err);
}

}  // namespace

void init_write(nb::module_& m) {
  m.attr("CODEC_NONE") = (int)FSTX_CODEC_NONE;
  m.attr("CODEC_LZ4") = (int)FSTX_CODEC_LZ4;
  m.attr("CODEC_ZSTD") = (int)FSTX_CODEC_ZSTD;
  m.def("write", &write, nb::arg("path"), nb::arg("columns"), nb::arg("table_attrs").none(), nb::arg("nrow"),
        nb::arg("codec"), nb::arg("level"), nb::arg("threads"), nb::arg("profile"), nb::arg("keep_pct"),
        nb::arg("append"),
        "Writes prepared columns (private: use fstx.write_fstx).");
}

}  // namespace fstxpy
