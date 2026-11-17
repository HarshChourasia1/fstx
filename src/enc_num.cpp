// FST-X encodings for fixed-width values (file format 3).
// Integers (int32, and int64 inside ALP / ALP-RD / double dictionaries): CONST, FOR, DELTA, RLE, DICT,
// SPLIT, PLAIN. Doubles: CONST, ALP, ALP-RD, DICT, SPLIT, PLAIN (the non-ALP ones work on the bit patterns).
// Bit-packing works on 1,024-value vectors in a lane-interleaved layout (lanes = 1024 / type bits), written
// as plain loops the compiler can vectorise; every vector has its own width and base.
// Clean-room code written from the published ideas. MIT licence. No R headers.
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <type_traits>
#include <utility>

#include "enc.h"
#include "simd.h"

namespace fstx {
namespace enc {

void corrupt(const char* what) { fail(FSTX_ERR_CORRUPT, "corrupt page (%s)", what); }

void check_aligned(const uint8_t* body) {
  if ((uintptr_t)body & 7u) fail(FSTX_ERR_INTERNAL, "page body is not 8-byte aligned");
}

const char* tag_name(int tag) {
  switch (tag) {
    case T_PLAIN: return "plain";
    case T_CONST: return "const";
    case T_FOR: return "for";
    case T_DELTA: return "delta";
    case T_RLE: return "rle";
    case T_DICT: return "dict";
    case T_SPLIT: return "split";
    case T_ALP: return "alp";
    case T_ALPRD: return "alprd";
    case T_FSST: return "fsst";
    default: return "";
  }
}

const char* kind_name(int kind) {
  switch (kind) {
    case K_INT: return "int";
    case K_DBL: return "double";
    case K_LGL: return "logical";
    case K_STR: return "string";
    case K_CODES: return "codes";
    case K_DICT: return "dictionary";
    default: return "";
  }
}

size_t zstd_est(const EncCtx& cx, const uint8_t* p, size_t n) {
  if (!cx.cc || n == 0) return n;
  FSTX_PROF_SCOPE(W_ZSTD_EST);
  size_t cap = compress_bound(FSTX_CODEC_ZSTD, n);
  std::vector<uint8_t> tmp(cap);
  return compress_page(*cx.cc, FSTX_CODEC_ZSTD, -1, p, n, tmp.data(), cap);
}

namespace {

using kern::kV;
constexpr uint32_t kSampleVectors = 4;  // the chooser tries encodings on 4 evenly spaced vectors of a page
constexpr int kNestMax = 2;             // RLE and DICT only at depth 0 and 1: chains are at most 3 deep
constexpr uint64_t kNaRealBits = 0x7FF00000000007A2ULL;

inline int bits_of(uint64_t r) { return r ? 64 - __builtin_clzll(r) : 0; }

template <class T>
using UT = typename std::make_unsigned<T>::type;
using kern::kB;

// bit-packing: kern_scalar.h (plain) and simd_kernels.cpp (SIMD), chosen at run time by kx::

// ---------------------------------------------------------------- FOR / DELTA
template <class T>
struct ForPlan {
  std::vector<uint8_t> w;
  std::vector<T> base;
  size_t packed = 0;
};

template <class T>
void for_plan(const T* x, uint32_t n, ForPlan<T>& fp) {
  FSTX_PROF_SCOPE(W_FORPLAN);
  using U = UT<T>;
  const uint32_t nv = (n + kV - 1) / kV;
  fp.w.resize(nv);
  fp.base.resize(nv);
  fp.packed = 0;
  for (uint32_t v = 0; v < nv; ++v) {
    const T* s = x + (size_t)v * kV;
    const uint32_t m = std::min(kV, n - v * kV);
    T mn, mx;
    kx::minmax<T>(s, m, mn, mx);
    fp.base[v] = mn;
    fp.w[v] = (uint8_t)bits_of((uint64_t)(U)((U)mx - (U)mn));
    fp.packed += 128u * fp.w[v];
  }
}

template <class T>
size_t for_size(const ForPlan<T>& fp, bool delta) {
  const size_t nv = fp.w.size();
  return 8 + a8(nv) + a8(nv * sizeof(T)) * (delta ? 2 : 1) + fp.packed;
}

template <class T>
void put_packed(const T* x, uint32_t n, const ForPlan<T>& fp, Buf& out) {
  using U = UT<T>;
  FSTX_PROF_SCOPE(W_PACK);
  uint8_t* dst = out.grow(fp.packed);
  alignas(64) U tmp[kV];
  const uint32_t nv = (uint32_t)fp.w.size();
  for (uint32_t v = 0; v < nv; ++v) {
    const T* s = x + (size_t)v * kV;
    const uint32_t m = std::min(kV, n - v * kV);
    const U b = (U)fp.base[v];
    for (uint32_t i = 0; i < m; ++i) tmp[i] = (U)((U)s[i] - b);
    for (uint32_t i = m; i < kV; ++i) tmp[i] = 0;
    kx::pack<U>(fp.w[v], tmp, (U*)dst);
    dst += 128u * fp.w[v];
  }
}

template <class T>
void make_deltas(const T* x, uint32_t n, std::vector<T>& d, std::vector<T>& firsts) {
  FSTX_PROF_SCOPE(W_DELTA_MAKE);
  using U = UT<T>;
  const uint32_t nv = (n + kV - 1) / kV;
  d.resize(n);
  firsts.resize(nv);
  for (uint32_t v = 0; v < nv; ++v) {
    const size_t i0 = (size_t)v * kV;
    const uint32_t m = std::min(kV, n - v * kV);
    firsts[v] = x[i0];
    for (uint32_t i = 1; i < m; ++i) d[i0 + i] = (T)(U)((U)x[i0 + i] - (U)x[i0 + i - 1]);
    d[i0] = m > 1 ? d[i0 + 1] : T(0);  // the first slot is ignored on decode: keep it inside the range
  }
}

template <class T>
uint32_t put_for(const T* x, uint32_t n, Buf& out) {
  ForPlan<T> fp;
  for_plan(x, n, fp);
  const size_t nv = fp.w.size();
  size_t h = out.open(T_FOR);
  out.put(fp.w.data(), nv);
  out.pad8();
  out.put(fp.base.data(), nv * sizeof(T));
  out.pad8();
  put_packed(x, n, fp, out);
  out.close(h);
  return T_FOR;
}

template <class T>
uint32_t put_delta(const T* x, uint32_t n, Buf& out) {
  std::vector<T> d, firsts;
  make_deltas(x, n, d, firsts);
  ForPlan<T> fp;
  for_plan(d.data(), n, fp);
  const size_t nv = fp.w.size();
  size_t h = out.open(T_DELTA);
  out.put(fp.w.data(), nv);
  out.pad8();
  out.put(firsts.data(), nv * sizeof(T));
  out.pad8();
  out.put(fp.base.data(), nv * sizeof(T));
  out.pad8();
  put_packed(d.data(), n, fp, out);
  out.close(h);
  return T_DELTA;
}

template <class T>
void get_for(const Blk& b, T* out, uint32_t n, bool delta) {
  using U = UT<T>;
  FSTX_PROF_BEGIN_E(prof_for, sizeof(T) == 4 ? ::fstx::prof::R_FOR32 : ::fstx::prof::R_FOR64);
  const uint32_t nv = (n + kV - 1) / kV;
  Cur c(b);
  const uint8_t* w = c.take(nv);
  c.pad();
  const T* firsts = nullptr;
  if (delta) {
    firsts = (const T*)c.take((size_t)nv * sizeof(T));
    c.pad();
  }
  const T* base = (const T*)c.take((size_t)nv * sizeof(T));
  c.pad();
  size_t total = 0;
  for (uint32_t v = 0; v < nv; ++v) {
    if (w[v] > kB<U>) corrupt("bit width");
    total += 128u * w[v];
  }
  const uint8_t* src = c.take(total);
  alignas(64) U tmp[kV];
  for (uint32_t v = 0; v < nv; ++v) {
    const uint32_t m = std::min(kV, n - v * kV);
    U* o = m == kV ? (U*)(out + (size_t)v * kV) : tmp;
    kx::unpack<U>(w[v], (const U*)src, o, (U)base[v]);
    if (delta) {
      FSTX_PROF_SCOPE(R_DELTA_SUM);
      kx::delta_sum<U>(o, m, (U)firsts[v]);
    }
    if (m < kV) std::memcpy(out + (size_t)v * kV, tmp, (size_t)m * sizeof(T));
    src += 128u * w[v];
  }
}

// ---------------------------------------------------------------- other integer encodings
template <class T>
uint32_t put_plain(const T* x, uint32_t n, Buf& out) {
  size_t h = out.open(T_PLAIN);
  out.put(x, (size_t)n * sizeof(T));
  out.close(h);
  return T_PLAIN;
}

template <class T>
uint32_t put_split(const T* x, uint32_t n, Buf& out) {
  constexpr int S = (int)sizeof(T);
  FSTX_PROF_SCOPE(W_SPLIT);
  size_t h = out.open(T_SPLIT, (uint8_t)S);
  uint8_t* d = out.grow((size_t)n * S);
  const uint8_t* s = (const uint8_t*)x;
  for (int b = 0; b < S; ++b) {
    uint8_t* o = d + (size_t)b * n;
    for (uint32_t i = 0; i < n; ++i) o[i] = s[(size_t)i * S + b];
  }
  out.close(h);
  return T_SPLIT;
}

template <class T>
void get_split(const Blk& b, T* out, uint32_t n) {
  constexpr int S = (int)sizeof(T);
  if (b.a != S || b.len != (size_t)n * S) corrupt("byte-split size");
  FSTX_PROF_SCOPE(R_SPLIT);
  kx::unsplit<S>(b.p, (uint8_t*)out, n);
}

template <class T>
uint32_t encode_auto(const T* x, uint32_t n, int depth, const EncCtx& cx, Buf& out);
template <class T>
int decode_blk(Rd& rd, T* out, uint32_t n, int depth);

template <class T>
uint32_t count_runs(const T* x, uint32_t n) {
  if (!n) return 0;
  uint32_t r = 1;
  for (uint32_t i = 1; i < n; ++i) r += x[i] != x[i - 1];
  return r;
}

template <class T>
uint32_t put_rle(const T* x, uint32_t n, int depth, const EncCtx& cx, Buf& out) {
  std::vector<T> vals;
  std::vector<int32_t> lens;
  for (uint32_t i = 0; i < n;) {
    uint32_t j = i + 1;
    while (j < n && x[j] == x[i]) ++j;
    vals.push_back(x[i]);
    lens.push_back((int32_t)(j - i));
    i = j;
  }
  const uint32_t nr = (uint32_t)vals.size();
  size_t h = out.open(T_RLE);
  put_u32(out.grow(8), nr);
  uint32_t c1 = encode_auto<T>(vals.data(), nr, depth + 1, cx, out) & 255u;
  uint32_t c2 = encode_auto<int32_t>(lens.data(), nr, depth + 1, cx, out) & 255u;
  out.close(h);
  return T_RLE | c1 << 8 | c2 << 16;
}

template <class T>
void get_rle(const Blk& b, T* out, uint32_t n, int depth) {
  if (depth >= kNestMax) corrupt("encodings nested too deep");
  FSTX_PROF_SCOPE(R_RLE);
  Cur c(b);
  const uint32_t nr = get_u32(c.take(8));
  if (nr > n || (n > 0 && nr == 0)) corrupt("run count");
  std::vector<T> vals(nr);
  std::vector<int32_t> lens(nr);
  Rd rd = c.rest();
  decode_blk<T>(rd, vals.data(), nr, depth + 1);
  decode_blk<int32_t>(rd, lens.data(), nr, depth + 1);
  uint64_t pos = 0;
  for (uint32_t r = 0; r < nr; ++r) {
    const int32_t l = lens[r];
    if (l <= 0 || pos + (uint64_t)l > n) corrupt("run length");
    std::fill(out + pos, out + pos + l, vals[r]);
    pos += (uint64_t)l;
  }
  if (pos != n) corrupt("run lengths do not add up");
}

// Dictionary of distinct values (sorted), codes = position in it. False when more than `limit` distinct.
template <class T>
bool build_dict(const T* x, uint32_t n, uint32_t limit, std::vector<T>& vals, std::vector<int32_t>& codes) {
  FSTX_PROF_SCOPE(W_DICT_BUILD);
  vals.clear();
  codes.resize(n);
  if (n == 0) return true;
  uint32_t cap = 16;
  int lg = 4;
  while (cap < 2u * limit + 2u) {
    cap <<= 1;
    ++lg;
  }
  std::vector<int32_t> slot(cap, -1);
  const uint32_t msk = cap - 1;
  for (uint32_t i = 0; i < n; ++i) {
    uint64_t key = 0;
    std::memcpy(&key, &x[i], sizeof(T));
    uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ULL) >> (64 - lg));
    while (slot[h] >= 0 && vals[(size_t)slot[h]] != x[i]) h = (h + 1) & msk;
    if (slot[h] < 0) {
      if (vals.size() >= limit) return false;
      slot[h] = (int32_t)vals.size();
      vals.push_back(x[i]);
    }
    codes[i] = slot[h];
  }
  // sort the dictionary so its values compress well (and the codes keep the values' order)
  const size_t nd = vals.size();
  std::vector<int32_t> order(nd), rank(nd);
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) { return vals[(size_t)a] < vals[(size_t)b]; });
  std::vector<T> sorted(nd);
  for (size_t k = 0; k < nd; ++k) {
    rank[(size_t)order[k]] = (int32_t)k;
    sorted[k] = vals[(size_t)order[k]];
  }
  vals.swap(sorted);
  for (uint32_t i = 0; i < n; ++i) codes[i] = rank[(size_t)codes[i]];
  return true;
}

template <class T>
uint32_t put_dict(const std::vector<T>& vals, const std::vector<int32_t>& codes, uint32_t n, int depth,
                  const EncCtx& cx, Buf& out) {
  const uint32_t nd = (uint32_t)vals.size();
  size_t h = out.open(T_DICT);
  put_u32(out.grow(8), nd);
  uint32_t c1 = encode_auto<T>(vals.data(), nd, depth + 1, cx, out) & 255u;
  uint32_t c2 = encode_auto<int32_t>(codes.data(), n, depth + 1, cx, out) & 255u;
  out.close(h);
  return T_DICT | c1 << 8 | c2 << 16;
}

template <class T>
void get_dict(const Blk& b, T* out, uint32_t n, int depth) {
  if (depth >= kNestMax) corrupt("encodings nested too deep");
  Cur c(b);
  const uint32_t nd = get_u32(c.take(8));
  if (nd > n || (n > 0 && nd == 0)) corrupt("dictionary size");
  std::vector<T> vals(nd);
  Rd rd = c.rest();
  decode_blk<T>(rd, vals.data(), nd, depth + 1);
  if constexpr (std::is_same<T, int32_t>::value) {
    decode_blk<int32_t>(rd, out, n, depth + 1);
    FSTX_PROF_SCOPE(R_DICT);
    if (!kx::dict_gather<int32_t>(out, n, vals.data(), nd, out)) corrupt("dictionary code");
  } else {
    std::vector<int32_t> codes(n);
    decode_blk<int32_t>(rd, codes.data(), n, depth + 1);
    FSTX_PROF_SCOPE(R_DICT);
    if (!kx::dict_gather<T>(codes.data(), n, vals.data(), nd, out)) corrupt("dictionary code");
  }
}

// ---------------------------------------------------------------- integer chooser
template <class T>
struct Sample {
  std::vector<T> own;
  const T* p = nullptr;
  uint32_t n = 0;
};

// whole vectors, evenly spaced; the whole page when it is small
template <class T>
void take_sample(const T* x, uint32_t n, Sample<T>& s) {
  const uint32_t nv = n / kV;
  if (nv <= 2 * kSampleVectors) {
    s.p = x;
    s.n = n;
    return;
  }
  s.own.resize((size_t)kSampleVectors * kV);
  for (uint32_t k = 0; k < kSampleVectors; ++k) {
    const uint32_t v = (uint32_t)((uint64_t)k * (nv - 1) / (kSampleVectors - 1));
    std::memcpy(s.own.data() + (size_t)k * kV, x + (size_t)v * kV, kV * sizeof(T));
  }
  s.p = s.own.data();
  s.n = kSampleVectors * kV;
}

template <class T>
bool is_const(const T* x, uint32_t n) {
  for (uint32_t i = 1; i < n; ++i)
    if (x[i] != x[0]) return false;
  return n > 0;
}

template <class T>
uint32_t encode_as(int tag, const T* x, uint32_t n, int depth, const EncCtx& cx, Buf& out) {
  switch (tag) {
    case T_CONST: {
      size_t h = out.open(T_CONST);
      out.put(x, sizeof(T));
      out.close(h);
      return T_CONST;
    }
    case T_FOR: return put_for(x, n, out);
    case T_DELTA: return put_delta(x, n, out);
    case T_RLE: return put_rle(x, n, depth, cx, out);
    case T_DICT: {
      std::vector<T> vals;
      std::vector<int32_t> codes;
      if (!build_dict(x, n, std::max<uint32_t>(1u, n / 2), vals, codes)) return put_for(x, n, out);
      return put_dict(vals, codes, n, depth, cx, out);
    }
    case T_SPLIT: return put_split(x, n, out);
    default: return put_plain(x, n, out);
  }
}

struct Choice {
  int tag = T_PLAIN;
  size_t size = 0;   // on the sample
  size_t plain = 0;  // plain size of the sample
  Buf buf;           // the sample encoded with `tag`, when it was built anyway
  bool has_buf = false;
};

template <class T>
void choose_int(const T* x, uint32_t n, int depth, const EncCtx& cx, Sample<T>& s, Choice& r) {
  r = Choice();
  if (n == 0) return;
  if (is_const(x, n)) {
    r.tag = T_CONST;
    return;
  }
  take_sample(x, n, s);
  r.plain = 8 + a8((size_t)s.n * sizeof(T));
  r.size = r.plain;
  ForPlan<T> fp;
  for_plan(s.p, s.n, fp);
  size_t sz = for_size(fp, false);
  if (sz < r.size) {
    r.size = sz;
    r.tag = T_FOR;
  }
  {
    std::vector<T> d, firsts;
    make_deltas(s.p, s.n, d, firsts);
    for_plan(d.data(), s.n, fp);
    sz = for_size(fp, true);
    if (sz < r.size) {
      r.size = sz;
      r.tag = T_DELTA;
    }
  }
  if (depth < kNestMax) {
    if (count_runs(s.p, s.n) * 2 <= s.n) {
      Buf b;
      put_rle(s.p, s.n, depth, cx, b);
      if (b.size() < r.size) {
        r.size = b.size();
        r.tag = T_RLE;
        r.buf = std::move(b);
        r.has_buf = true;
      }
    }
    std::vector<T> vals;
    std::vector<int32_t> codes;
    if (build_dict(s.p, s.n, s.n / 4, vals, codes)) {
      Buf b;
      put_dict(vals, codes, s.n, depth, cx, b);
      if (b.size() < r.size) {
        r.size = b.size();
        r.tag = T_DICT;
        r.buf = std::move(b);
        r.has_buf = true;
      }
    }
  }
  if (r.has_buf && (r.tag != T_RLE && r.tag != T_DICT)) r.has_buf = false;
}

template <class T>
uint32_t encode_auto(const T* x, uint32_t n, int depth, const EncCtx& cx, Buf& out) {
  Sample<T> s;
  Choice c;
  choose_int(x, n, depth, cx, s, c);
  return encode_as(c.tag, x, n, depth, cx, out);
}

template <class T>
int decode_blk(Rd& rd, T* out, uint32_t n, int depth) {
  if (depth > kNestMax) corrupt("encodings nested too deep");
  Blk b = rd.next();
  switch (b.tag) {
    case T_PLAIN:
      if (b.len != (size_t)n * sizeof(T)) corrupt("plain size");
      if (n) std::memcpy(out, b.p, (size_t)n * sizeof(T));
      break;
    case T_CONST: {
      if (b.len != sizeof(T)) corrupt("constant size");
      T v;
      std::memcpy(&v, b.p, sizeof(T));
      std::fill(out, out + n, v);
      break;
    }
    case T_FOR: get_for(b, out, n, false); break;
    case T_DELTA: get_for(b, out, n, true); break;
    case T_RLE: get_rle(b, out, n, depth); break;
    case T_DICT: get_dict(b, out, n, depth); break;
    case T_SPLIT: get_split(b, out, n); break;
    default: corrupt("unknown integer encoding");
  }
  return b.tag;
}

// ---------------------------------------------------------------- partial decode (M5 scattered rows)
// vm holds one byte per 1,024-value vector (nonzero = needed). Only needed vectors are written to out; the
// others are left as they were. Encodings without vector-level access (RLE) decode the whole block. vm == NULL
// means every vector (the normal whole-block decoder).
template <class T>
void get_for_part(const Blk& b, T* out, uint32_t n, bool delta, const uint8_t* vm) {
  using U = UT<T>;
  const uint32_t nv = (n + kV - 1) / kV;
  Cur c(b);
  const uint8_t* w = c.take(nv);
  c.pad();
  const T* firsts = nullptr;
  if (delta) {
    firsts = (const T*)c.take((size_t)nv * sizeof(T));
    c.pad();
  }
  const T* base = (const T*)c.take((size_t)nv * sizeof(T));
  c.pad();
  size_t total = 0;
  for (uint32_t v = 0; v < nv; ++v) {
    if (w[v] > kB<U>) corrupt("bit width");
    total += 128u * w[v];
  }
  const uint8_t* src = c.take(total);
  alignas(64) U tmp[kV];
  for (uint32_t v = 0; v < nv; ++v) {
    if (vm[v]) {
      const uint32_t m = std::min(kV, n - v * kV);
      U* o = m == kV ? (U*)(out + (size_t)v * kV) : tmp;
      kx::unpack<U>(w[v], (const U*)src, o, (U)base[v]);
      if (delta) kx::delta_sum<U>(o, m, (U)firsts[v]);
      if (m < kV) std::memcpy(out + (size_t)v * kV, tmp, (size_t)m * sizeof(T));
    }
    src += 128u * w[v];
  }
}

template <class T>
int decode_blk_part(Rd& rd, T* out, uint32_t n, int depth, const uint8_t* vm);

template <class T>
void get_dict_part(const Blk& b, T* out, uint32_t n, int depth, const uint8_t* vm) {
  if (depth >= kNestMax) corrupt("encodings nested too deep");
  Cur c(b);
  const uint32_t nd = get_u32(c.take(8));
  if (nd > n || (n > 0 && nd == 0)) corrupt("dictionary size");
  std::vector<T> vals(nd);
  Rd rd = c.rest();
  decode_blk<T>(rd, vals.data(), nd, depth + 1);
  const uint32_t nv = (n + kV - 1) / kV;
  if constexpr (std::is_same<T, int32_t>::value) {
    decode_blk_part<int32_t>(rd, out, n, depth + 1, vm);
    for (uint32_t v = 0; v < nv; ++v) {
      if (!vm[v]) continue;
      const size_t o = (size_t)v * kV;
      const uint32_t m = std::min(kV, n - v * kV);
      if (!kx::dict_gather<int32_t>(out + o, m, vals.data(), nd, out + o)) corrupt("dictionary code");
    }
  } else {
    std::vector<int32_t> codes(n);
    decode_blk_part<int32_t>(rd, codes.data(), n, depth + 1, vm);
    for (uint32_t v = 0; v < nv; ++v) {
      if (!vm[v]) continue;
      const size_t o = (size_t)v * kV;
      const uint32_t m = std::min(kV, n - v * kV);
      if (!kx::dict_gather<T>(codes.data() + o, m, vals.data(), nd, out + o)) corrupt("dictionary code");
    }
  }
}

template <class T>
int decode_blk_part(Rd& rd, T* out, uint32_t n, int depth, const uint8_t* vm) {
  if (!vm) return decode_blk<T>(rd, out, n, depth);
  if (depth > kNestMax) corrupt("encodings nested too deep");
  const int tag = rd.peek();
  const uint32_t nv = (n + kV - 1) / kV;
  switch (tag) {
    case T_PLAIN: {
      Blk b = rd.next();
      if (b.len != (size_t)n * sizeof(T)) corrupt("plain size");
      for (uint32_t v = 0; v < nv; ++v)
        if (vm[v])
          std::memcpy(out + (size_t)v * kV, b.p + (size_t)v * kV * sizeof(T), (size_t)std::min(kV, n - v * kV) * sizeof(T));
      return tag;
    }
    case T_CONST: {
      Blk b = rd.next();
      if (b.len != sizeof(T)) corrupt("constant size");
      T x;
      std::memcpy(&x, b.p, sizeof(T));
      for (uint32_t v = 0; v < nv; ++v)
        if (vm[v]) std::fill(out + (size_t)v * kV, out + (size_t)v * kV + std::min(kV, n - v * kV), x);
      return tag;
    }
    case T_FOR:
    case T_DELTA: {
      Blk b = rd.next();
      get_for_part(b, out, n, tag == T_DELTA, vm);
      return tag;
    }
    case T_SPLIT: {
      Blk b = rd.next();
      constexpr int S = (int)sizeof(T);
      if (b.a != S || b.len != (size_t)n * S) corrupt("byte-split size");
      for (uint32_t v = 0; v < nv; ++v) {
        if (!vm[v]) continue;
        const uint32_t i0 = v * kV, m = std::min(kV, n - i0);
        uint8_t* o = (uint8_t*)(out + i0);
        for (uint32_t i = 0; i < m; ++i)
          for (int k = 0; k < S; ++k) o[(size_t)i * S + k] = b.p[(size_t)k * n + i0 + i];
      }
      return tag;
    }
    case T_DICT: {
      Blk b = rd.next();
      get_dict_part(b, out, n, depth, vm);
      return tag;
    }
    default:
      return decode_blk<T>(rd, out, n, depth);  // RLE (and anything else): the whole block
  }
}

// ---------------------------------------------------------------- NA section
template <class T, class IsNa>
int na_write(const T* x, uint32_t n, IsNa isna, Buf& out, uint32_t& nna) {
  // one pass, no branches: NA count and the number of NA / non-NA changes (runs = changes + 1)
  uint32_t cnt = 0, chg = 0, prev = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t a = isna(x[i]) ? 1u : 0u;
    cnt += a;
    chg += a ^ prev;
    prev = a;
  }
  nna = cnt;
  if (!nna) return NA_NONE;
  const size_t bm = 8 * (((size_t)n + 63) / 64), rs = 8 + a8(4 * ((size_t)chg + 1));
  if (rs < bm) {
    // runs alternate non-NA / NA, starting with a (possibly empty) non-NA run
    std::vector<uint32_t> runs;
    runs.reserve((size_t)chg + 1);
    bool cur = false;
    uint32_t len = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const bool a = isna(x[i]);
      if (a != cur) {
        runs.push_back(len);
        len = 0;
        cur = a;
      }
      ++len;
    }
    runs.push_back(len);
    size_t h = out.open(T_NA_RUNS);
    put_u32(out.grow(8), (uint32_t)runs.size());
    out.put(runs.data(), 4 * runs.size());
    out.close(h);
    return NA_RUNS;
  }
  size_t h = out.open(T_NA_BITMAP);
  uint64_t* w = (uint64_t*)out.grow(bm);
  for (uint32_t k = 0; (size_t)k * 64 < n; ++k) {  // whole words, no branches
    const uint32_t i0 = k * 64, m = std::min<uint32_t>(64, n - i0);
    uint64_t b = 0;
    for (uint32_t j = 0; j < m; ++j) b |= (uint64_t)(isna(x[i0 + j]) ? 1u : 0u) << j;
    w[k] = b;
  }
  out.close(h);
  return NA_BITMAP;
}

// NA slots take the previous value (leading NAs the first value), so FOR ranges stay small
template <class T, class IsNa>
void na_fill(const T* x, uint32_t n, IsNa isna, std::vector<T>& filled) {
  filled.assign(x, x + n);
  T last{};
  for (uint32_t i = 0; i < n; ++i)
    if (!isna(x[i])) {
      last = x[i];
      break;
    }
  for (uint32_t i = 0; i < n; ++i) {
    if (isna(filled[i])) filled[i] = last;
    else last = filled[i];
  }
}

template <class T>
void na_apply(const Blk& b, int mode, uint32_t n, T* out, T na, const T* must_be) {
  FSTX_PROF_SCOPE(R_NA);
  if (mode == NA_BITMAP) {
    const size_t nw = ((size_t)n + 63) / 64;
    if (b.tag != T_NA_BITMAP || b.len != 8 * nw) corrupt("NA bitmap");
    const uint64_t* w = (const uint64_t*)b.p;
    size_t k0 = 0;
    if (!must_be) {  // whole words hold rows < n only: the kernel needs no checks
      k0 = (size_t)n / 64;
      kx::na_bitmap<T>(w, k0, out, na);
    }
    for (size_t k = k0; k < nw; ++k) {
      uint64_t m = w[k];
      while (m) {
        const size_t i = k * 64 + (size_t)__builtin_ctzll(m);
        if (i >= n) corrupt("NA bitmap");
        if (must_be && std::memcmp(&out[i], must_be, sizeof(T)) != 0) corrupt("value under an NA");
        out[i] = na;
        m &= m - 1;
      }
    }
  } else if (mode == NA_RUNS) {
    if (b.tag != T_NA_RUNS) corrupt("NA runs");
    Cur c(b);
    const uint32_t nr = get_u32(c.take(8));
    const uint32_t* r = (const uint32_t*)c.take((size_t)nr * 4);
    uint64_t pos = 0;
    bool isna = false;
    for (uint32_t k = 0; k < nr; ++k) {
      if (pos + r[k] > n) corrupt("NA runs");
      if (isna)
        for (uint64_t i = pos; i < pos + r[k]; ++i) {
          if (must_be && std::memcmp(&out[i], must_be, sizeof(T)) != 0) corrupt("value under an NA");
          out[i] = na;
        }
      pos += r[k];
      isna = !isna;
    }
    if (pos != n) corrupt("NA runs do not add up");
  } else {
    corrupt("NA mode");
  }
}

// NA block applied to the needed vectors only (vm as for decode_blk_part)
template <class T>
void na_apply_part(const Blk& b, int mode, uint32_t n, T* out, T na, const uint8_t* vm) {
  if (!vm) return na_apply<T>(b, mode, n, out, na, nullptr);
  const uint32_t nv = (n + kV - 1) / kV;
  if (mode == NA_BITMAP) {
    const size_t nw = ((size_t)n + 63) / 64;
    if (b.tag != T_NA_BITMAP || b.len != 8 * nw) corrupt("NA bitmap");
    const uint64_t* w = (const uint64_t*)b.p;
    for (uint32_t v = 0; v < nv; ++v) {
      if (!vm[v]) continue;
      const size_t k1 = std::min(nw, (size_t)(v + 1) * (kV / 64));
      for (size_t k = (size_t)v * (kV / 64); k < k1; ++k) {
        uint64_t m = w[k];
        while (m) {
          const size_t i = k * 64 + (size_t)__builtin_ctzll(m);
          if (i >= n) corrupt("NA bitmap");
          out[i] = na;
          m &= m - 1;
        }
      }
    }
  } else if (mode == NA_RUNS) {
    if (b.tag != T_NA_RUNS) corrupt("NA runs");
    Cur c(b);
    const uint32_t nr = get_u32(c.take(8));
    const uint32_t* r = (const uint32_t*)c.take((size_t)nr * 4);
    uint64_t pos = 0;
    bool isna = false;
    for (uint32_t k = 0; k < nr; ++k) {
      if (pos + r[k] > n) corrupt("NA runs");
      if (isna && r[k]) {
        const uint64_t e = pos + r[k];
        for (uint64_t v = pos / kV; v * kV < e; ++v) {
          if (!vm[v]) continue;
          const uint64_t a = std::max<uint64_t>(pos, v * kV), z = std::min<uint64_t>(e, (v + 1) * kV);
          std::fill(out + a, out + z, na);
        }
      }
      pos += r[k];
      isna = !isna;
    }
    if (pos != n) corrupt("NA runs do not add up");
  } else {
    corrupt("NA mode");
  }
}

// ---------------------------------------------------------------- top level for int32 pages
uint32_t encode_top_int(const int32_t* x, uint32_t n, const EncCtx& cx0, Buf& out) {
  FSTX_PROF_SCOPE(W_INT_TOP);
  EncCtx cx = cx0;
  cx.force = 0;
  const int f = cx0.force;
  if (f && n > 0) {
    if (f == T_CONST) {
      if (is_const(x, n)) return encode_as<int32_t>(T_CONST, x, n, 0, cx, out);
    } else if (f == T_FOR || f == T_DELTA || f == T_RLE || f == T_DICT || f == T_SPLIT || f == T_PLAIN) {
      return encode_as<int32_t>(f, x, n, 0, cx, out);
    }
  }
  Sample<int32_t> s;
  Choice c;
  FSTX_PROF_BEGIN(prof_choose, W_CHOOSE);
  choose_int(x, n, 0, cx, s, c);
  int tag = c.tag;
  if (cx.codec != FSTX_CODEC_NONE && tag != T_CONST && n > 0 && c.size * 2 > c.plain && s.n >= kV) {
    // weak lightweight result: would byte-split + codec do better? Estimate both with ZSTD -1 on the sample.
    Buf a;
    if (c.has_buf) a = std::move(c.buf);
    else encode_as<int32_t>(tag, s.p, s.n, 0, cx, a);
    const size_t za = zstd_est(cx, a.v.data(), a.size());
    Buf b;
    put_split(s.p, s.n, b);
    const size_t zb = zstd_est(cx, b.v.data(), b.size());
    if (zb < std::min(a.size(), za)) tag = T_SPLIT;
  }
  FSTX_PROF_END(prof_choose);
  return encode_as<int32_t>(tag, x, n, 0, cx, out);
}

// ---------------------------------------------------------------- doubles: ALP
using kern::kF10;
using kern::alp_dec;

inline bool alp_enc(double v, int e, int f, int64_t& q) {
  const double t = (v * kF10[e]) / kF10[f];
  if (!(t > -2251799813685248.0 && t < 2251799813685248.0)) return false;  // |t| < 2^51 (NaN fails too)
  const double r = (t + 6755399441055744.0) - 6755399441055744.0;            // round to nearest
  q = (int64_t)r;
  const double back = alp_dec(q, e, f);
  uint64_t a, b;
  std::memcpy(&a, &back, 8);
  std::memcpy(&b, &v, 8);
  return a == b;
}

struct AlpParams {
  int e = 0, f = 0;
};

// best (e, f) on up to 64 values spread over the page; false when every pair fails on a quarter of them
bool alp_search(const double* x, uint32_t n, AlpParams& p) {
  FSTX_PROF_SCOPE(W_ALP_SEARCH);
  if (n == 0) return false;
  const uint32_t ns = std::min<uint32_t>(n, 64);
  double s[64];
  for (uint32_t k = 0; k < ns; ++k) s[k] = x[(uint64_t)k * n / ns];
  size_t best = std::numeric_limits<size_t>::max();
  for (int e = 0; e <= 18; ++e) {
    for (int f = 0; f <= e; ++f) {
      uint32_t exc = 0;
      int64_t mn = std::numeric_limits<int64_t>::max(), mx = std::numeric_limits<int64_t>::min();
      bool bad = false;
      for (uint32_t k = 0; k < ns; ++k) {
        int64_t q;
        if (alp_enc(s[k], e, f, q)) {
          mn = std::min(mn, q);
          mx = std::max(mx, q);
        } else if (++exc > ns / 4) {
          bad = true;
          break;
        }
      }
      if (bad || mn > mx) continue;
      const size_t cost = (size_t)ns * (size_t)bits_of((uint64_t)mx - (uint64_t)mn) + (size_t)exc * 96;
      if (cost < best) {
        best = cost;
        p.e = e;
        p.f = f;
      }
    }
  }
  return best != std::numeric_limits<size_t>::max();
}

uint32_t put_alp(const double* x, uint32_t n, const AlpParams& ap, int depth, const EncCtx& cx, Buf& out) {
  std::vector<int64_t> q(n);
  std::vector<uint32_t> pos;
  std::vector<uint64_t> raw;
  int64_t last = 0;
  FSTX_PROF_BEGIN(prof_alp, W_ALP_ENC);
  for (uint32_t i = 0; i < n; ++i) {  // exceptions before the first good value take its integer
    int64_t k;
    if (alp_enc(x[i], ap.e, ap.f, k)) {
      last = k;
      break;
    }
  }
  for (uint32_t i = 0; i < n; ++i) {
    int64_t k;
    if (alp_enc(x[i], ap.e, ap.f, k)) {
      q[i] = k;
      last = k;
    } else {
      q[i] = last;
      pos.push_back(i);
      uint64_t b;
      std::memcpy(&b, &x[i], 8);
      raw.push_back(b);
    }
  }
  const uint32_t ne = (uint32_t)pos.size();
  FSTX_PROF_END(prof_alp);
  size_t h = out.open(T_ALP, (uint8_t)ap.e, (uint8_t)ap.f);
  put_u32(out.grow(8), ne);
  out.put(pos.data(), 4ull * ne);
  out.pad8();
  out.put(raw.data(), 8ull * ne);
  uint32_t c1 = encode_auto<int64_t>(q.data(), n, depth + 1, cx, out) & 255u;
  out.close(h);
  return T_ALP | c1 << 8;
}

void get_alp(const Blk& b, double* out, uint32_t n, int depth) {
  const int e = b.a, f = b.b;
  if (e > 18 || f > e) corrupt("ALP exponents");
  Cur c(b);
  const uint32_t ne = get_u32(c.take(8));
  if (ne > n) corrupt("ALP exceptions");
  const uint32_t* pos = (const uint32_t*)c.take(4ull * ne);
  c.pad();
  const uint8_t* raw = c.take(8ull * ne);
  Rd rd = c.rest();
  decode_blk<int64_t>(rd, (int64_t*)out, n, depth + 1);
  FSTX_PROF_SCOPE(R_ALP);
  kx::alp_decode(out, n, e, f);
  for (uint32_t k = 0; k < ne; ++k) {
    if (pos[k] >= n) corrupt("ALP exception position");
    std::memcpy(out + pos[k], raw + 8ull * k, 8);
  }
}

// ---------------------------------------------------------------- doubles: ALP-RD
struct RdParams {
  int lb = 8;  // left bits (1..16)
  int nd = 1;
  uint16_t dict[8] = {0};
};

void alprd_search(const double* x, uint32_t n, RdParams& p) {
  FSTX_PROF_SCOPE(W_ALPRD_SEARCH);
  const uint32_t ns = std::min<uint32_t>(n, 256);
  std::vector<uint64_t> s(ns);
  for (uint32_t k = 0; k < ns; ++k) std::memcpy(&s[k], &x[(uint64_t)k * n / ns], 8);
  // sorted once: the top lb bits of sorted 64-bit words are sorted too, for every lb
  std::sort(s.begin(), s.end());
  std::vector<uint16_t> left(ns);
  std::vector<std::pair<uint32_t, uint16_t>> cnt;
  size_t best = std::numeric_limits<size_t>::max();
  for (int lb = 1; lb <= 16; ++lb) {
    const int rb = 64 - lb;
    for (uint32_t k = 0; k < ns; ++k) left[k] = (uint16_t)(s[k] >> rb);
    cnt.clear();
    for (uint32_t k = 0; k < ns;) {
      uint32_t j = k;
      while (j < ns && left[j] == left[k]) ++j;
      cnt.push_back({j - k, left[k]});
      k = j;
    }
    const int nd = (int)std::min<size_t>(8, cnt.size());
    // only the first nd count: the order is total (values are distinct), so this is the full sort's prefix
    std::partial_sort(cnt.begin(), cnt.begin() + nd, cnt.end(),
                      [](const std::pair<uint32_t, uint16_t>& a, const std::pair<uint32_t, uint16_t>& b) {
                        return a.first != b.first ? a.first > b.first : a.second < b.second;
                      });
    uint32_t covered = 0;
    for (int k = 0; k < nd; ++k) covered += cnt[(size_t)k].first;
    const size_t cost = (size_t)ns * (size_t)(rb + bits_of((uint64_t)(nd - 1))) + (size_t)(ns - covered) * 48;
    if (cost < best) {
      best = cost;
      p.lb = lb;
      p.nd = nd;
      for (int k = 0; k < 8; ++k) p.dict[k] = k < nd ? cnt[(size_t)k].second : 0;
    }
  }
}

uint32_t put_alprd(const double* x, uint32_t n, const RdParams& p, int depth, const EncCtx& cx, Buf& out) {
  const int rb = 64 - p.lb;
  const uint64_t mask = (rb >= 64) ? ~0ull : ((1ull << rb) - 1);
  std::vector<int32_t> codes(n);
  std::vector<int64_t> rights(n);
  std::vector<uint32_t> pos;
  std::vector<uint16_t> lefts;
  for (uint32_t i = 0; i < n; ++i) {
    uint64_t bits;
    std::memcpy(&bits, &x[i], 8);
    const uint16_t l = (uint16_t)(bits >> rb);
    int code = -1;
    for (int k = 0; k < p.nd; ++k)
      if (p.dict[k] == l) {
        code = k;
        break;
      }
    if (code < 0) {
      code = 0;
      pos.push_back(i);
      lefts.push_back(l);
    }
    codes[i] = code;
    rights[i] = (int64_t)(bits & mask);
  }
  const uint32_t ne = (uint32_t)pos.size();
  size_t h = out.open(T_ALPRD, (uint8_t)p.lb, (uint8_t)p.nd);
  out.put(p.dict, 16);
  put_u32(out.grow(8), ne);
  out.put(pos.data(), 4ull * ne);
  out.pad8();
  out.put(lefts.data(), 2ull * ne);
  out.pad8();
  uint32_t c1 = encode_auto<int32_t>(codes.data(), n, depth + 1, cx, out) & 255u;
  uint32_t c2 = encode_auto<int64_t>(rights.data(), n, depth + 1, cx, out) & 255u;
  out.close(h);
  return T_ALPRD | c1 << 8 | c2 << 16;
}

void get_alprd(const Blk& b, double* out, uint32_t n, int depth) {
  const int lb = b.a, nd = b.b;
  if (lb < 1 || lb > 16 || nd < 1 || nd > 8) corrupt("ALP-RD parameters");
  const int rb = 64 - lb;
  const uint64_t mask = (1ull << rb) - 1;
  Cur c(b);
  const uint8_t* dp = c.take(16);
  uint16_t dict[8];
  std::memcpy(dict, dp, 16);
  const uint32_t ne = get_u32(c.take(8));
  if (ne > n) corrupt("ALP-RD exceptions");
  const uint32_t* pos = (const uint32_t*)c.take(4ull * ne);
  c.pad();
  const uint16_t* lefts = (const uint16_t*)c.take(2ull * ne);
  c.pad();
  Rd rd = c.rest();
  std::vector<int32_t> codes(n);
  decode_blk<int32_t>(rd, codes.data(), n, depth + 1);
  decode_blk<int64_t>(rd, (int64_t*)out, n, depth + 1);
  FSTX_PROF_SCOPE(R_ALPRD);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t k = (uint32_t)codes[i];
    if (k >= (uint32_t)nd) corrupt("ALP-RD code");
    uint64_t r;
    std::memcpy(&r, out + i, 8);
    const uint64_t bits = ((uint64_t)dict[k] << rb) | (r & mask);
    std::memcpy(out + i, &bits, 8);
  }
  for (uint32_t k = 0; k < ne; ++k) {
    if (pos[k] >= n) corrupt("ALP-RD exception position");
    uint64_t r;
    std::memcpy(&r, out + pos[k], 8);
    const uint64_t bits = ((uint64_t)lefts[k] << rb) | (r & mask);
    std::memcpy(out + pos[k], &bits, 8);
  }
}

// ---------------------------------------------------------------- doubles: partial decode (M5)
int decode_dblk(Rd& rd, double* out, uint32_t n, int depth);

void get_alp_part(const Blk& b, double* out, uint32_t n, int depth, const uint8_t* vm) {
  const int e = b.a, f = b.b;
  if (e > 18 || f > e) corrupt("ALP exponents");
  Cur c(b);
  const uint32_t ne = get_u32(c.take(8));
  if (ne > n) corrupt("ALP exceptions");
  const uint32_t* pos = (const uint32_t*)c.take(4ull * ne);
  c.pad();
  const uint8_t* raw = c.take(8ull * ne);
  Rd rd = c.rest();
  decode_blk_part<int64_t>(rd, (int64_t*)out, n, depth + 1, vm);
  const uint32_t nv = (n + kV - 1) / kV;
  for (uint32_t v = 0; v < nv; ++v)
    if (vm[v]) kx::alp_decode(out + (size_t)v * kV, std::min(kV, n - v * kV), e, f);
  for (uint32_t k = 0; k < ne; ++k) {
    if (pos[k] >= n) corrupt("ALP exception position");
    if (vm[pos[k] / kV]) std::memcpy(out + pos[k], raw + 8ull * k, 8);
  }
}

void get_alprd_part(const Blk& b, double* out, uint32_t n, int depth, const uint8_t* vm) {
  const int lb = b.a, nd = b.b;
  if (lb < 1 || lb > 16 || nd < 1 || nd > 8) corrupt("ALP-RD parameters");
  const int rb = 64 - lb;
  const uint64_t mask = (1ull << rb) - 1;
  Cur c(b);
  const uint8_t* dp = c.take(16);
  uint16_t dict[8];
  std::memcpy(dict, dp, 16);
  const uint32_t ne = get_u32(c.take(8));
  if (ne > n) corrupt("ALP-RD exceptions");
  const uint32_t* pos = (const uint32_t*)c.take(4ull * ne);
  c.pad();
  const uint16_t* lefts = (const uint16_t*)c.take(2ull * ne);
  c.pad();
  Rd rd = c.rest();
  std::vector<int32_t> codes(n);
  decode_blk_part<int32_t>(rd, codes.data(), n, depth + 1, vm);
  decode_blk_part<int64_t>(rd, (int64_t*)out, n, depth + 1, vm);
  const uint32_t nv = (n + kV - 1) / kV;
  for (uint32_t v = 0; v < nv; ++v) {
    if (!vm[v]) continue;
    const uint32_t i1 = v * kV + std::min(kV, n - v * kV);
    for (uint32_t i = v * kV; i < i1; ++i) {
      const uint32_t k = (uint32_t)codes[i];
      if (k >= (uint32_t)nd) corrupt("ALP-RD code");
      uint64_t r;
      std::memcpy(&r, out + i, 8);
      const uint64_t bits = ((uint64_t)dict[k] << rb) | (r & mask);
      std::memcpy(out + i, &bits, 8);
    }
  }
  for (uint32_t k = 0; k < ne; ++k) {
    if (pos[k] >= n) corrupt("ALP-RD exception position");
    if (!vm[pos[k] / kV]) continue;
    uint64_t r;
    std::memcpy(&r, out + pos[k], 8);
    const uint64_t bits = ((uint64_t)lefts[k] << rb) | (r & mask);
    std::memcpy(out + pos[k], &bits, 8);
  }
}

int decode_dblk_part(Rd& rd, double* out, uint32_t n, int depth, const uint8_t* vm) {
  const int tag = rd.peek();
  if (vm && (tag == T_ALP || tag == T_ALPRD)) {
    Blk b = rd.next();
    if (tag == T_ALP) get_alp_part(b, out, n, depth, vm);
    else get_alprd_part(b, out, n, depth, vm);
    return tag;
  }
  if (!vm) return decode_dblk(rd, out, n, depth);
  return decode_blk_part<int64_t>(rd, (int64_t*)out, n, depth, vm);
}

// ---------------------------------------------------------------- doubles: top level
int decode_dblk(Rd& rd, double* out, uint32_t n, int depth) {
  const int tag = rd.peek();
  if (tag == T_ALP || tag == T_ALPRD) {
    Blk b = rd.next();
    if (tag == T_ALP) get_alp(b, out, n, depth);
    else get_alprd(b, out, n, depth);
    return tag;
  }
  return decode_blk<int64_t>(rd, (int64_t*)out, n, depth);  // the other encodings work on the bit patterns
}

uint32_t encode_top_dbl(const double* x, uint32_t n, const EncCtx& cx0, Buf& out) {
  FSTX_PROF_SCOPE(W_DBL_TOP);
  EncCtx cx = cx0;
  cx.force = 0;
  const int64_t* xb = (const int64_t*)x;
  if (n == 0) return put_plain(xb, 0, out);
  const bool cst = is_const(xb, n);
  const int f = cx0.force;
  if (f) {
    if (f == T_CONST && cst) return encode_as<int64_t>(T_CONST, xb, n, 0, cx, out);
    // FOR, DELTA and RLE work on the bit patterns; the chooser never picks them for doubles, but they are legal
    if (f == T_PLAIN || f == T_SPLIT || f == T_DICT || f == T_FOR || f == T_DELTA || f == T_RLE)
      return encode_as<int64_t>(f, xb, n, 0, cx, out);
    if (f == T_ALP) {
      AlpParams ap;
      alp_search(x, n, ap);
      return put_alp(x, n, ap, 0, cx, out);
    }
    if (f == T_ALPRD) {
      RdParams rp;
      alprd_search(x, n, rp);
      return put_alprd(x, n, rp, 0, cx, out);
    }
  }
  if (cst) return encode_as<int64_t>(T_CONST, xb, n, 0, cx, out);

  FSTX_PROF_BEGIN(prof_choose, W_CHOOSE);
  Sample<int64_t> s;
  take_sample(xb, n, s);
  const double* sd = (const double*)s.p;
  const size_t plain = 8 + 8 * (size_t)s.n;
  size_t best = plain;
  int tag = T_PLAIN;
  Buf bestbuf;
  bool has_buf = false;
  AlpParams ap;
  RdParams rp;
  if (alp_search(x, n, ap)) {
    Buf b;
    put_alp(sd, s.n, ap, 0, cx, b);
    if (b.size() < best) {
      best = b.size();
      tag = T_ALP;
      bestbuf = std::move(b);
      has_buf = true;
    }
  }
  if (best * 10 > plain * 7) {
    alprd_search(x, n, rp);
    Buf b;
    put_alprd(sd, s.n, rp, 0, cx, b);
    if (b.size() < best) {
      best = b.size();
      tag = T_ALPRD;
      bestbuf = std::move(b);
      has_buf = true;
    }
  }
  {
    std::vector<int64_t> vals;
    std::vector<int32_t> codes;
    if (build_dict(s.p, s.n, s.n / 4, vals, codes)) {
      Buf b;
      put_dict(vals, codes, s.n, 0, cx, b);
      if (b.size() < best) {
        best = b.size();
        tag = T_DICT;
        bestbuf = std::move(b);
        has_buf = true;
      }
    }
  }
  if (cx.codec != FSTX_CODEC_NONE && best * 2 > plain && s.n >= kV) {
    Buf a;
    if (has_buf && tag != T_PLAIN) a = std::move(bestbuf);
    else put_plain(s.p, s.n, a);
    const size_t za = zstd_est(cx, a.v.data(), a.size());
    Buf b;
    put_split(s.p, s.n, b);
    const size_t zb = zstd_est(cx, b.v.data(), b.size());
    if (zb < std::min(a.size(), za)) tag = T_SPLIT;
  }
  FSTX_PROF_END(prof_choose);
  switch (tag) {
    case T_ALP: return put_alp(x, n, ap, 0, cx, out);
    case T_ALPRD: return put_alprd(x, n, rp, 0, cx, out);
    default: return encode_as<int64_t>(tag, xb, n, 0, cx, out);
  }
}

// Is there any NA (int) / NaN (double) at all? 16-byte vectors (GCC / Clang vector extensions), 64 values between
// tests. Pages without one skip na_write's scans, which would write nothing for them.
#if defined(_MSC_VER) && !defined(__clang__)
// Microsoft cl.exe has no vector extensions: plain loops (Python build only; R builds with GCC or Clang).
bool any_int_na(const int32_t* x, uint32_t n, int32_t na_value) {
  for (uint32_t i = 0; i < n; ++i)
    if (x[i] == na_value) return true;
  return false;
}

bool any_nan(const double* x, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i)
    if (x[i] != x[i]) return true;
  return false;
}
#else
typedef double vd2 __attribute__((vector_size(16)));
typedef int64_t vl2 __attribute__((vector_size(16)));
typedef int32_t vi4 __attribute__((vector_size(16)));

bool any_int_na(const int32_t* x, uint32_t n, int32_t na_value) {
  const vi4 vna = {na_value, na_value, na_value, na_value};
  uint32_t i = 0;
  for (; i + 64 <= n; i += 64) {
    vi4 acc = {0, 0, 0, 0};
    for (uint32_t k = 0; k < 64; k += 4) {
      vi4 v;
      std::memcpy(&v, x + i + k, 16);
      acc |= v == vna;
    }
    if (acc[0] | acc[1] | acc[2] | acc[3]) return true;
  }
  for (; i < n; ++i)
    if (x[i] == na_value) return true;
  return false;
}

bool any_nan(const double* x, uint32_t n) {
  uint32_t i = 0;
  for (; i + 64 <= n; i += 64) {
    vl2 acc = {0, 0};
    for (uint32_t k = 0; k < 64; k += 2) {
      vd2 v;
      std::memcpy(&v, x + i + k, 16);
      acc |= (vl2)(v != v);
    }
    if (acc[0] | acc[1]) return true;
  }
  for (; i < n; ++i)
    if (x[i] != x[i]) return true;
  return false;
}

#endif  // vector extensions

void write_header(Buf& out, int kind, uint32_t n) {
  uint8_t* h = out.grow(8);
  h[0] = (uint8_t)kind;
  put_u32(h + 4, n);
}

void check_header(const uint8_t* body, size_t len, uint32_t chain, uint32_t n, int kind) {
  check_aligned(body);
  if (len < 8 || (len & 7u)) corrupt("page size");
  if (body[0] != kind || chain_kind(chain) != kind) corrupt("page kind does not match the column");
  if (body[1] != chain_na(chain) || body[1] > NA_RUNS) corrupt("NA mode");
  if (get_u32(body + 4) != n) corrupt("row count");
}

}  // namespace

// ---------------------------------------------------------------- public
uint32_t encode_int32_auto(const int32_t* x, uint32_t n, int depth, const EncCtx& cx, Buf& out) {
  return encode_auto<int32_t>(x, n, depth, cx, out);
}

int decode_int32_blk(Rd& rd, int32_t* out, uint32_t n, int depth) { return decode_blk<int32_t>(rd, out, n, depth); }

int na_write_i32(const int32_t* x, uint32_t n, int32_t na_value, Buf& out) {
  uint32_t nna;
  return na_write(x, n, [na_value](int32_t v) { return v == na_value; }, out, nna);
}

void na_apply_i32(const Blk& b, int mode, uint32_t n, int32_t* out, int32_t na, const int32_t* must_be) {
  na_apply<int32_t>(b, mode, n, out, na, must_be);
}

uint32_t encode_int_page(const int32_t* x, uint32_t n, int kind, int32_t na_value, const EncCtx& cx, Buf& out) {
  out.clear();
  out.v.reserve((size_t)n * 4 + 256);
  write_header(out, kind, n);
  uint32_t nna;
  auto isna = [na_value](int32_t v) { return v == na_value; };
  FSTX_PROF_BEGIN(prof_na, W_NA);
  const int na = any_int_na(x, n, na_value) ? na_write(x, n, isna, out, nna) : NA_NONE;
  out.v[1] = (uint8_t)na;
  std::vector<int32_t> filled;
  if (na) na_fill(x, n, isna, filled);
  FSTX_PROF_END(prof_na);
  const int32_t* xs = na ? filled.data() : x;
  const size_t vstart = out.size();
  uint32_t tags = encode_top_int(xs, n, cx, out);
  if (!cx.force && (tags & 255u) != T_PLAIN && out.size() - vstart > 8 + a8((size_t)n * 4)) {
    out.v.resize(vstart);  // never bigger than plain
    tags = put_plain(xs, n, out);
  }
  return make_chain(kind, na, tags);
}

uint32_t encode_dbl_page(const double* x, uint32_t n, const EncCtx& cx, Buf& out) {
  out.clear();
  out.v.reserve((size_t)n * 8 + 256);
  write_header(out, K_DBL, n);
  uint32_t nna;
  auto isna = [](double v) {
    uint64_t b;
    std::memcpy(&b, &v, 8);
    return b == kNaRealBits;
  };
  FSTX_PROF_BEGIN(prof_na, W_NA);
  const int na = any_nan(x, n) ? na_write(x, n, isna, out, nna) : NA_NONE;  // R's NA is a NaN
  out.v[1] = (uint8_t)na;
  std::vector<double> filled;
  if (na) na_fill(x, n, isna, filled);
  FSTX_PROF_END(prof_na);
  const double* xs = na ? filled.data() : x;
  const size_t vstart = out.size();
  uint32_t tags = encode_top_dbl(xs, n, cx, out);
  if (!cx.force && (tags & 255u) != T_PLAIN && out.size() - vstart > 8 + (size_t)n * 8) {
    out.v.resize(vstart);
    tags = put_plain((const int64_t*)xs, n, out);
  }
  return make_chain(K_DBL, na, tags);
}

void decode_int_page(const uint8_t* body, size_t len, uint32_t chain, uint32_t n, int kind, int32_t na_value,
                     int32_t* out) {
  FSTX_PROF_SCOPE(R_DEC_INT);
  check_header(body, len, chain, n, kind);
  Rd rd{body + 8, body + len};
  Blk nb{};
  const int na = body[1];
  if (na) nb = rd.next();
  if (rd.peek() != chain_tag(chain, 0)) corrupt("encoding does not match the page directory");
  decode_blk<int32_t>(rd, out, n, 0);
  if (!rd.done()) corrupt("trailing bytes");
  if (na) na_apply<int32_t>(nb, na, n, out, na_value, nullptr);
}

void decode_dbl_page(const uint8_t* body, size_t len, uint32_t chain, uint32_t n, double* out) {
  FSTX_PROF_SCOPE(R_DEC_DBL);
  check_header(body, len, chain, n, K_DBL);
  Rd rd{body + 8, body + len};
  Blk nb{};
  const int na = body[1];
  if (na) nb = rd.next();
  if (rd.peek() != chain_tag(chain, 0)) corrupt("encoding does not match the page directory");
  decode_dblk(rd, out, n, 0);
  if (!rd.done()) corrupt("trailing bytes");
  if (na) {
    double nav;
    std::memcpy(&nav, &kNaRealBits, 8);
    na_apply<double>(nb, na, n, out, nav, nullptr);
  }
}

void decode_int_page_part(const uint8_t* body, size_t len, uint32_t chain, uint32_t n, int kind, int32_t na_value,
                          const uint8_t* vm, int32_t* out) {
  if (!vm) return decode_int_page(body, len, chain, n, kind, na_value, out);
  check_header(body, len, chain, n, kind);
  Rd rd{body + 8, body + len};
  Blk nb{};
  const int na = body[1];
  if (na) nb = rd.next();
  if (rd.peek() != chain_tag(chain, 0)) corrupt("encoding does not match the page directory");
  decode_blk_part<int32_t>(rd, out, n, 0, vm);
  if (!rd.done()) corrupt("trailing bytes");
  if (na) na_apply_part<int32_t>(nb, na, n, out, na_value, vm);
}

void decode_dbl_page_part(const uint8_t* body, size_t len, uint32_t chain, uint32_t n, const uint8_t* vm,
                          double* out) {
  if (!vm) return decode_dbl_page(body, len, chain, n, out);
  check_header(body, len, chain, n, K_DBL);
  Rd rd{body + 8, body + len};
  Blk nb{};
  const int na = body[1];
  if (na) nb = rd.next();
  if (rd.peek() != chain_tag(chain, 0)) corrupt("encoding does not match the page directory");
  decode_dblk_part(rd, out, n, 0, vm);
  if (!rd.done()) corrupt("trailing bytes");
  if (na) {
    double nav;
    std::memcpy(&nav, &kNaRealBits, 8);
    na_apply_part<double>(nb, na, n, out, nav, vm);
  }
}

}  // namespace enc
}  // namespace fstx
