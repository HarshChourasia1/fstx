// FST-X SIMD kernels (M4), written with Google Highway (vendored, BSD-3) and compiled once per x86 target
// (SSE2, SSE4, AVX2; AVX-512 "AVX3" is left out in Makevars until tested), the best one the CPU runs being chosen
// at run time. Each kernel matches its plain version in kern_scalar.h bit for bit; see simd.h.
// Clean-room code, MIT licence. No R headers.
#include <algorithm>
#include <cstring>
#include <utility>

#include "simd.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "simd_kernels.cpp"
#include "hwy/foreach_target.h"  // IWYU pragma: keep
#include "hwy/highway.h"

HWY_BEFORE_NAMESPACE();
namespace fstx {
namespace simd {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// ---------------------------------------------------------------- bit-packing (lane-interleaved, see kern_scalar.h)
template <class U, int W>
void UnpackW(const U* HWY_RESTRICT in, U* HWY_RESTRICT out, U base) {
  constexpr int B = (int)sizeof(U) * 8, L = 1024 / B;
  const hn::ScalableTag<U> d;
  const size_t N = hn::Lanes(d);
  const auto vb = hn::Set(d, base);
  if constexpr (W == 0) {
    (void)in;
    for (size_t i = 0; i < 1024; i += N) hn::StoreU(vb, d, out + i);
  } else {
    constexpr U mask = (W == B) ? (U)~U(0) : (U)((U(1) << (W % B)) - 1);
    const auto vm = hn::Set(d, mask);
    for (int r = 0; r < B; ++r) {
      const int bit = r * W, w = bit / B, off = bit % B;
      const U* s = in + w * L;
      U* o = out + r * L;
      if (off + W > B) {
        const U* s2 = s + L;
        for (size_t l = 0; l < (size_t)L; l += N) {
          const auto a = hn::ShiftRightSame(hn::LoadU(d, s + l), off);
          const auto b = hn::ShiftLeftSame(hn::LoadU(d, s2 + l), B - off);
          hn::StoreU(hn::Add(hn::And(hn::Or(a, b), vm), vb), d, o + l);
        }
      } else {
        for (size_t l = 0; l < (size_t)L; l += N)
          hn::StoreU(hn::Add(hn::And(hn::ShiftRightSame(hn::LoadU(d, s + l), off), vm), vb), d, o + l);
      }
    }
  }
}

template <class U, int W>
void PackW(const U* HWY_RESTRICT in, U* HWY_RESTRICT out) {
  constexpr int B = (int)sizeof(U) * 8, L = 1024 / B;
  if constexpr (W > 0) {
    const hn::ScalableTag<U> d;
    const size_t N = hn::Lanes(d);
    const auto z = hn::Zero(d);
    for (size_t i = 0; i < (size_t)(W * L); i += N) hn::StoreU(z, d, out + i);
    for (int r = 0; r < B; ++r) {
      const int bit = r * W, w = bit / B, off = bit % B;
      U* o = out + w * L;
      const U* x = in + r * L;
      for (size_t l = 0; l < (size_t)L; l += N) {
        const auto v = hn::LoadU(d, x + l);
        hn::StoreU(hn::Or(hn::LoadU(d, o + l), hn::ShiftLeftSame(v, off)), d, o + l);
        if (off + W > B) {
          U* o2 = o + L;
          hn::StoreU(hn::Or(hn::LoadU(d, o2 + l), hn::ShiftRightSame(v, B - off)), d, o2 + l);
        }
      }
    }
  } else {
    (void)in;
    (void)out;
  }
}

template <class U>
using UnpFn = void (*)(const U*, U*, U);
template <class U>
using PkFn = void (*)(const U*, U*);

template <class U, size_t... I>
const UnpFn<U>* UnpTable(std::index_sequence<I...>) {
  static const UnpFn<U> t[] = {&UnpackW<U, (int)I>...};
  return t;
}
template <class U, size_t... I>
const PkFn<U>* PkTable(std::index_sequence<I...>) {
  static const PkFn<U> t[] = {&PackW<U, (int)I>...};
  return t;
}

void Unpack32(int w, const uint32_t* in, uint32_t* out, uint32_t base) {
  UnpTable<uint32_t>(std::make_index_sequence<33>())[w](in, out, base);
}
void Unpack64(int w, const uint64_t* in, uint64_t* out, uint64_t base) {
  UnpTable<uint64_t>(std::make_index_sequence<65>())[w](in, out, base);
}
void Pack32(int w, const uint32_t* in, uint32_t* out) { PkTable<uint32_t>(std::make_index_sequence<33>())[w](in, out); }
void Pack64(int w, const uint64_t* in, uint64_t* out) { PkTable<uint64_t>(std::make_index_sequence<65>())[w](in, out); }

// ---------------------------------------------------------------- DELTA prefix sum (wrap-around arithmetic)
// int32: 128-bit vectors on every target (in-block shifts are cheap; a 256-bit scan needs lane-crossing shuffles
// and measured slower than the plain loop). int64: the plain loop (2 lanes per vector do not pay; measured).
void Delta32(uint32_t* HWY_RESTRICT o, uint32_t m, uint32_t first) {
  if (m == 0) return;
  o[0] = first;
  size_t i = 1;
#if HWY_TARGET != HWY_SCALAR
  const hn::CappedTag<uint32_t, 4> d;
  if (hn::Lanes(d) == 4 && m >= 8) {
    auto carry = hn::Zero(d);
    for (i = 0; i + 4 <= m; i += 4) {
      auto v = hn::LoadU(d, o + i);
      v = hn::Add(v, hn::ShiftLeftLanes<1>(d, v));
      v = hn::Add(v, hn::ShiftLeftLanes<2>(d, v));
      v = hn::Add(v, carry);
      hn::StoreU(v, d, o + i);
      carry = hn::Broadcast<3>(v);
    }
  }
#endif
  for (; i < m; ++i) o[i] = o[i] + o[i - 1];
}
void Delta64(uint64_t* HWY_RESTRICT o, uint32_t m, uint64_t first) { kern::delta_sum<uint64_t>(o, m, first); }

// ---------------------------------------------------------------- min and max (FOR plan)
// int64 min / max have no SSE instructions (emulated: slower than the plain loop, measured): AVX2 and up only.
template <class T>
HWY_INLINE void MinMaxT(const T* HWY_RESTRICT s, uint32_t m, T& lo, T& hi) {
  const hn::ScalableTag<T> d;
  const size_t N = hn::Lanes(d);
  if (m < N || N == 1 || (sizeof(T) == 8 && HWY_TARGET > HWY_AVX2)) {
    kern::minmax<T>(s, m, lo, hi);
    return;
  }
  auto vmn = hn::LoadU(d, s), vmx = vmn;
  size_t i = N;
  for (; i + N <= m; i += N) {
    const auto v = hn::LoadU(d, s + i);
    vmn = hn::Min(vmn, v);
    vmx = hn::Max(vmx, v);
  }
  if (i < m) {  // overlapping last vector
    const auto v = hn::LoadU(d, s + m - N);
    vmn = hn::Min(vmn, v);
    vmx = hn::Max(vmx, v);
  }
  lo = hn::ReduceMin(d, vmn);
  hi = hn::ReduceMax(d, vmx);
}
void MinMax32(const int32_t* s, uint32_t m, int32_t& lo, int32_t& hi) { MinMaxT<int32_t>(s, m, lo, hi); }
void MinMax64(const int64_t* s, uint32_t m, int64_t& lo, int64_t& hi) { MinMaxT<int64_t>(s, m, lo, hi); }

// ---------------------------------------------------------------- ALP decode, in place: int64 q -> q * 10^f / 10^e
// int64 -> double: exact through the 1.5 * 2^52 trick when |q| < 2^51. Each block of up to 256 values is first
// checked; a block with a larger q (only in a corrupt or hand-made file) takes the plain conversion. Multiply
// then divide as the plain code does (no FMA), so the result is the same double.
void AlpDecode(double* HWY_RESTRICT out, uint32_t n, int e, int f) {
  const hn::ScalableTag<double> dd;
  const hn::RebindToSigned<decltype(dd)> di;
  const hn::RebindToUnsigned<decltype(dd)> du;
  const size_t N = hn::Lanes(dd);
  const auto vmul = hn::Set(dd, kern::kF10[f]), vdiv = hn::Set(dd, kern::kF10[e]);
  const auto magic_i = hn::Set(di, (int64_t)0x4338000000000000LL);
  const auto magic_d = hn::Set(dd, 6755399441055744.0);
  const auto off = hn::Set(du, (uint64_t)1 << 51);
  size_t i = 0;
  while (i + N <= n) {
    const size_t blk = std::min<size_t>(256, (n - i) / N * N);
    const int64_t* q = (const int64_t*)(out + i);
    auto bad = hn::Zero(du);  // (q + 2^51) >> 52 is 0 exactly when -2^51 <= q < 2^51
    for (size_t k = 0; k < blk; k += N)
      bad = hn::Or(bad, hn::ShiftRight<52>(hn::Add(hn::BitCast(du, hn::LoadU(di, q + k)), off)));
    if (!hn::AllTrue(du, hn::Eq(bad, hn::Zero(du)))) {
      kern::alp_decode(out + i, (uint32_t)blk, e, f);
    } else {
      for (size_t k = 0; k < blk; k += N) {
        const auto x = hn::Sub(hn::BitCast(dd, hn::Add(hn::LoadU(di, q + k), magic_i)), magic_d);
        hn::StoreU(hn::Div(hn::Mul(x, vmul), vdiv), dd, out + i + k);
      }
    }
    i += blk;
  }
  if (i < n) kern::alp_decode(out + i, (uint32_t)(n - i), e, f);
}

// ---------------------------------------------------------------- byte-split decode (S streams -> values)
void Unsplit4(const uint8_t* HWY_RESTRICT src, uint8_t* HWY_RESTRICT o, uint32_t n) {
  size_t i = 0;
#if HWY_TARGET != HWY_SCALAR
  const hn::Full128<uint8_t> d8;
  const hn::Full128<uint16_t> d16;
  const uint8_t *s0 = src, *s1 = src + n, *s2 = src + 2 * (size_t)n, *s3 = src + 3 * (size_t)n;
  for (; i + 16 <= n; i += 16) {
    const auto a0 = hn::LoadU(d8, s0 + i), a1 = hn::LoadU(d8, s1 + i);
    const auto a2 = hn::LoadU(d8, s2 + i), a3 = hn::LoadU(d8, s3 + i);
    const auto t0 = hn::BitCast(d16, hn::InterleaveLower(d8, a0, a1));
    const auto t1 = hn::BitCast(d16, hn::InterleaveUpper(d8, a0, a1));
    const auto t2 = hn::BitCast(d16, hn::InterleaveLower(d8, a2, a3));
    const auto t3 = hn::BitCast(d16, hn::InterleaveUpper(d8, a2, a3));
    uint8_t* p = o + 4 * i;
    hn::StoreU(hn::BitCast(d8, hn::InterleaveLower(d16, t0, t2)), d8, p);
    hn::StoreU(hn::BitCast(d8, hn::InterleaveUpper(d16, t0, t2)), d8, p + 16);
    hn::StoreU(hn::BitCast(d8, hn::InterleaveLower(d16, t1, t3)), d8, p + 32);
    hn::StoreU(hn::BitCast(d8, hn::InterleaveUpper(d16, t1, t3)), d8, p + 48);
  }
#endif
  for (int k = 0; k < 4; ++k) {
    const uint8_t* s = src + (size_t)k * n;
    for (size_t j = i; j < n; ++j) o[j * 4 + k] = s[j];
  }
}

void Unsplit8(const uint8_t* HWY_RESTRICT src, uint8_t* HWY_RESTRICT o, uint32_t n) {
  size_t i = 0;
#if HWY_TARGET != HWY_SCALAR
  const hn::Full128<uint8_t> d8;
  const hn::Full128<uint16_t> d16;
  const hn::Full128<uint32_t> d32;
  for (; i + 16 <= n; i += 16) {
    hn::Vec<decltype(d8)> a[8];
    for (int k = 0; k < 8; ++k) a[k] = hn::LoadU(d8, src + (size_t)k * n + i);
    // bytes -> pairs
    const auto L0 = hn::BitCast(d16, hn::InterleaveLower(d8, a[0], a[1]));
    const auto U0 = hn::BitCast(d16, hn::InterleaveUpper(d8, a[0], a[1]));
    const auto L1 = hn::BitCast(d16, hn::InterleaveLower(d8, a[2], a[3]));
    const auto U1 = hn::BitCast(d16, hn::InterleaveUpper(d8, a[2], a[3]));
    const auto L2 = hn::BitCast(d16, hn::InterleaveLower(d8, a[4], a[5]));
    const auto U2 = hn::BitCast(d16, hn::InterleaveUpper(d8, a[4], a[5]));
    const auto L3 = hn::BitCast(d16, hn::InterleaveLower(d8, a[6], a[7]));
    const auto U3 = hn::BitCast(d16, hn::InterleaveUpper(d8, a[6], a[7]));
    // pairs -> quads (bytes 0-3 and 4-7 of each value)
    const auto q0 = hn::BitCast(d32, hn::InterleaveLower(d16, L0, L1));
    const auto q1 = hn::BitCast(d32, hn::InterleaveUpper(d16, L0, L1));
    const auto q2 = hn::BitCast(d32, hn::InterleaveLower(d16, U0, U1));
    const auto q3 = hn::BitCast(d32, hn::InterleaveUpper(d16, U0, U1));
    const auto r0 = hn::BitCast(d32, hn::InterleaveLower(d16, L2, L3));
    const auto r1 = hn::BitCast(d32, hn::InterleaveUpper(d16, L2, L3));
    const auto r2 = hn::BitCast(d32, hn::InterleaveLower(d16, U2, U3));
    const auto r3 = hn::BitCast(d32, hn::InterleaveUpper(d16, U2, U3));
    uint8_t* p = o + 8 * i;
    hn::StoreU(hn::BitCast(d8, hn::InterleaveLower(d32, q0, r0)), d8, p);
    hn::StoreU(hn::BitCast(d8, hn::InterleaveUpper(d32, q0, r0)), d8, p + 16);
    hn::StoreU(hn::BitCast(d8, hn::InterleaveLower(d32, q1, r1)), d8, p + 32);
    hn::StoreU(hn::BitCast(d8, hn::InterleaveUpper(d32, q1, r1)), d8, p + 48);
    hn::StoreU(hn::BitCast(d8, hn::InterleaveLower(d32, q2, r2)), d8, p + 64);
    hn::StoreU(hn::BitCast(d8, hn::InterleaveUpper(d32, q2, r2)), d8, p + 80);
    hn::StoreU(hn::BitCast(d8, hn::InterleaveLower(d32, q3, r3)), d8, p + 96);
    hn::StoreU(hn::BitCast(d8, hn::InterleaveUpper(d32, q3, r3)), d8, p + 112);
  }
#endif
  for (int k = 0; k < 8; ++k) {
    const uint8_t* s = src + (size_t)k * n;
    for (size_t j = i; j < n; ++j) o[j * 8 + k] = s[j];
  }
}

// ---------------------------------------------------------------- NA bitmap: set bit -> NA value (nw full words)
template <class U>
HWY_INLINE void NaBitmapT(const uint64_t* HWY_RESTRICT w, size_t nw, U* HWY_RESTRICT out, U na) {
  const hn::ScalableTag<U> d;
  const size_t N = hn::Lanes(d);
  const auto bits = hn::Shl(hn::Set(d, U(1)), hn::Iota(d, 0));  // lane j holds 1 << j
  const auto vna = hn::Set(d, na);
  const uint64_t cm = N >= 64 ? ~0ull : ((1ull << N) - 1);
  for (size_t k = 0; k < nw; ++k) {
    uint64_t m = w[k];
    if (!m) continue;
    U* o = out + k * 64;
    if (__builtin_popcountll(m) <= 8) {  // few NAs in this word: the bit loop is faster (measured)
      while (m) {
        o[__builtin_ctzll(m)] = na;
        m &= m - 1;
      }
      continue;
    }
    for (size_t j = 0; j < 64; j += N) {
      const uint64_t c = (m >> j) & cm;
      if (!c) continue;
      const auto mk = hn::TestBit(hn::Set(d, (U)c), bits);
      hn::StoreU(hn::IfThenElse(mk, vna, hn::LoadU(d, o + j)), d, o + j);
    }
  }
}
void NaBitmap32(const uint64_t* w, size_t nw, int32_t* out, int32_t na) {
  NaBitmapT<uint32_t>(w, nw, (uint32_t*)out, (uint32_t)na);
}
// 64-bit lanes on SSE (2 per vector) measured slower than the bit loop: AVX2 and up only
void NaBitmap64(const uint64_t* w, size_t nw, uint64_t* out, uint64_t na) {
  if (HWY_TARGET > HWY_AVX2) kern::na_bitmap<uint64_t>(w, nw, out, na);
  else NaBitmapT<uint64_t>(w, nw, out, na);
}

// ---------------------------------------------------------------- dictionary gather (codes checked first)
// ---- int32: the plain loop on every target. Hardware gathers (vpgatherdd) measured slower than scalar loads on
// the test CPU (Arrow Lake; gathers are microcoded since the GDS mitigation), and SSE has no gather at all.
bool Dict32(const int32_t* codes, uint32_t n, const int32_t* vals, uint32_t nd, int32_t* out) {
  return kern::dict_gather<int32_t>(codes, n, vals, nd, out);
}

// ---- int64 (double dictionaries): AVX2 gather and up; SSE targets use the plain loop (measured slower).
bool Dict64(const int32_t* codes, uint32_t n, const int64_t* vals, uint32_t nd, int64_t* out) {
  if (HWY_TARGET > HWY_AVX2) return kern::dict_gather<int64_t>(codes, n, vals, nd, out);
  const hn::ScalableTag<int64_t> d;
  const hn::Rebind<int32_t, decltype(d)> d32;
  const hn::RebindToUnsigned<decltype(d32)> du32;
  const size_t N = hn::Lanes(d);
  const auto lim = hn::Set(du32, nd);
  size_t i = 0;
  for (; i + N <= n; i += N) {
    const auto c = hn::LoadU(d32, codes + i);
    if (!hn::AllTrue(du32, hn::Lt(hn::BitCast(du32, c), lim))) return false;
    hn::StoreU(hn::GatherIndex(d, vals, hn::PromoteTo(d, c)), d, out + i);
  }
  for (; i < n; ++i) {
    const uint32_t k = (uint32_t)codes[i];
    if (k >= nd) return false;
    out[i] = vals[k];
  }
  return true;
}

// the target this copy was compiled for (shows which copy the dispatcher really calls)
int64_t WhichTarget() { return HWY_TARGET; }

}  // namespace HWY_NAMESPACE
}  // namespace simd
}  // namespace fstx
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace fstx {
namespace simd {

std::atomic<bool> g_on{true};

HWY_EXPORT(Unpack32);
HWY_EXPORT(Unpack64);
HWY_EXPORT(Pack32);
HWY_EXPORT(Pack64);
HWY_EXPORT(Delta32);
HWY_EXPORT(Delta64);
HWY_EXPORT(MinMax32);
HWY_EXPORT(MinMax64);
HWY_EXPORT(AlpDecode);
HWY_EXPORT(Unsplit4);
HWY_EXPORT(Unsplit8);
HWY_EXPORT(NaBitmap32);
HWY_EXPORT(NaBitmap64);
HWY_EXPORT(Dict32);
HWY_EXPORT(Dict64);
HWY_EXPORT(WhichTarget);

int64_t dispatched_target() { return HWY_DYNAMIC_DISPATCH(WhichTarget)(); }

void unpack32(int w, const uint32_t* in, uint32_t* out, uint32_t base) {
  HWY_DYNAMIC_DISPATCH(Unpack32)(w, in, out, base);
}
void unpack64(int w, const uint64_t* in, uint64_t* out, uint64_t base) {
  HWY_DYNAMIC_DISPATCH(Unpack64)(w, in, out, base);
}
void pack32(int w, const uint32_t* in, uint32_t* out) { HWY_DYNAMIC_DISPATCH(Pack32)(w, in, out); }
void pack64(int w, const uint64_t* in, uint64_t* out) { HWY_DYNAMIC_DISPATCH(Pack64)(w, in, out); }
void delta32(uint32_t* o, uint32_t m, uint32_t first) { HWY_DYNAMIC_DISPATCH(Delta32)(o, m, first); }
void delta64(uint64_t* o, uint32_t m, uint64_t first) { HWY_DYNAMIC_DISPATCH(Delta64)(o, m, first); }
void minmax32(const int32_t* s, uint32_t m, int32_t& lo, int32_t& hi) { HWY_DYNAMIC_DISPATCH(MinMax32)(s, m, lo, hi); }
void minmax64(const int64_t* s, uint32_t m, int64_t& lo, int64_t& hi) { HWY_DYNAMIC_DISPATCH(MinMax64)(s, m, lo, hi); }
void alp_decode(double* out, uint32_t n, int e, int f) { HWY_DYNAMIC_DISPATCH(AlpDecode)(out, n, e, f); }
void unsplit4(const uint8_t* src, uint8_t* o, uint32_t n) { HWY_DYNAMIC_DISPATCH(Unsplit4)(src, o, n); }
void unsplit8(const uint8_t* src, uint8_t* o, uint32_t n) { HWY_DYNAMIC_DISPATCH(Unsplit8)(src, o, n); }
void na_bitmap32(const uint64_t* w, size_t nw, int32_t* out, int32_t na) {
  HWY_DYNAMIC_DISPATCH(NaBitmap32)(w, nw, out, na);
}
void na_bitmap64(const uint64_t* w, size_t nw, uint64_t* out, uint64_t na) {
  HWY_DYNAMIC_DISPATCH(NaBitmap64)(w, nw, out, na);
}
bool dict32(const int32_t* codes, uint32_t n, const int32_t* vals, uint32_t nd, int32_t* out) {
  return HWY_DYNAMIC_DISPATCH(Dict32)(codes, n, vals, nd, out);
}
bool dict64(const int32_t* codes, uint32_t n, const int64_t* vals, uint32_t nd, int64_t* out) {
  return HWY_DYNAMIC_DISPATCH(Dict64)(codes, n, vals, nd, out);
}

namespace {
int64_t real_cpu_targets() {  // what the CPU supports, ignoring any forced mask
  static const int64_t t = [] {
    hwy::SetSupportedTargetsForTest(0);
    return hwy::SupportedTargets();
  }();
  return t;
}
int64_t g_mask = 0;  // 0 = no forced mask
}  // namespace

int64_t compiled_targets() { return (int64_t)(HWY_TARGETS); }
int64_t cpu_targets() { return real_cpu_targets() & compiled_targets(); }
int64_t active_target() {
  if (!on()) return 0;
  const int64_t t = cpu_targets() & (g_mask ? g_mask : ~0LL);
  return t & -t;  // lowest bit = best target
}
const char* target_name(int64_t t) { return hwy::TargetName(t); }

bool set_mode(int mode, int64_t mask) {
  if (mode == 0) {
    g_on.store(false);
    return true;
  }
  int64_t use = 0;
  if (mode == 2) {
    use = mask & cpu_targets();
    if (!use) return false;
  }
  (void)real_cpu_targets();
  hwy::SetSupportedTargetsForTest(use);  // 0 = the CPU's own targets
  g_mask = use;
  g_on.store(true);
  return true;
}

}  // namespace simd
}  // namespace fstx
#endif  // HWY_ONCE
