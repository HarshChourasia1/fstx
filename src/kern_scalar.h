// FST-X hot loops, plain C++ (M4). These are the reference versions: the SIMD kernels in simd_kernels.cpp must
// give bit-identical results, and the decoder falls back to these when SIMD is off. Moved out of enc_num.cpp
// unchanged apart from the wrappers. Clean-room code, MIT licence. No R headers.
#ifndef FSTX_KERN_SCALAR_H
#define FSTX_KERN_SCALAR_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace fstx {
namespace kern {

constexpr uint32_t kV = 1024;  // values per bit-packed vector
template <class U>
constexpr int kB = (int)sizeof(U) * 8;
template <class U>
constexpr int kL = 1024 / kB<U>;

inline constexpr double kF10[19] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8, 1e9,
                                    1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18};

// ---------------------------------------------------------------- bit-packing
// Value i of a vector sits in lane (i % L), row (i / L). Each lane's rows are packed one after another
// into words L apart, so packing and unpacking run the same operation over all L lanes at once.
template <class U, int W>
void pack_w(const U* in, U* out) {
  constexpr int B = kB<U>, L = kL<U>;
  if constexpr (W > 0) {
    for (int i = 0; i < W * L; ++i) out[i] = 0;
    for (int r = 0; r < B; ++r) {
      const int bit = r * W, w = bit / B, off = bit % B;
      U* o = out + w * L;
      const U* x = in + r * L;
      for (int l = 0; l < L; ++l) o[l] |= (U)(x[l] << off);
      if (off + W > B) {
        U* o2 = o + L;
        for (int l = 0; l < L; ++l) o2[l] |= (U)(x[l] >> (B - off));
      }
    }
  } else {
    (void)in;
    (void)out;
  }
}

template <class U, int W>
void unpack_w(const U* in, U* out, U base) {
  constexpr int B = kB<U>, L = kL<U>;
  if constexpr (W == 0) {
    (void)in;
    for (int i = 0; i < 1024; ++i) out[i] = base;
  } else {
    constexpr U mask = (W == B) ? (U)~U(0) : (U)((U(1) << (W % B)) - 1);
    for (int r = 0; r < B; ++r) {
      const int bit = r * W, w = bit / B, off = bit % B;
      const U* s = in + w * L;
      U* o = out + r * L;
      if (off + W > B) {
        const U* s2 = s + L;
        for (int l = 0; l < L; ++l) o[l] = (U)((U)(((s[l] >> off) | (U)(s2[l] << (B - off))) & mask) + base);
      } else {
        for (int l = 0; l < L; ++l) o[l] = (U)((U)((s[l] >> off) & mask) + base);
      }
    }
  }
}

template <class U>
using PackFn = void (*)(const U*, U*);
template <class U>
using UnpackFn = void (*)(const U*, U*, U);

template <class U, size_t... I>
const PackFn<U>* pack_table(std::index_sequence<I...>) {
  static const PackFn<U> t[] = {&pack_w<U, (int)I>...};
  return t;
}
template <class U, size_t... I>
const UnpackFn<U>* unpack_table(std::index_sequence<I...>) {
  static const UnpackFn<U> t[] = {&unpack_w<U, (int)I>...};
  return t;
}
template <class U>
const PackFn<U>* packers() {
  return pack_table<U>(std::make_index_sequence<kB<U> + 1>());
}
template <class U>
const UnpackFn<U>* unpackers() {
  return unpack_table<U>(std::make_index_sequence<kB<U> + 1>());
}

// 1,024 values at width w (0..bits of U) plus base
template <class U>
inline void unpack(int w, const U* in, U* out, U base) {
  unpackers<U>()[w](in, out, base);
}
template <class U>
inline void pack(int w, const U* in, U* out) {
  packers<U>()[w](in, out);
}

// ---------------------------------------------------------------- DELTA: o[0] = first, then running sum
template <class U>
inline void delta_sum(U* o, uint32_t m, U first) {
  o[0] = first;
  for (uint32_t i = 1; i < m; ++i) o[i] = (U)(o[i] + o[i - 1]);
}

// ---------------------------------------------------------------- FOR plan: min and max of a vector (m >= 1)
template <class T>
inline void minmax(const T* s, uint32_t m, T& lo, T& hi) {
  T mn = s[0], mx = s[0];
  for (uint32_t i = 1; i < m; ++i) {
    mn = s[i] < mn ? s[i] : mn;
    mx = s[i] > mx ? s[i] : mx;
  }
  lo = mn;
  hi = mx;
}

// ---------------------------------------------------------------- ALP: q * 10^f / 10^e, in place (int64 -> double)
inline double alp_dec(int64_t q, int e, int f) { return ((double)q * kF10[f]) / kF10[e]; }

inline void alp_decode(double* out, uint32_t n, int e, int f) {
  for (uint32_t i = 0; i < n; ++i) {
    int64_t q;
    std::memcpy(&q, out + i, 8);
    out[i] = alp_dec(q, e, f);
  }
}

// ---------------------------------------------------------------- byte-split: S streams of n bytes -> n values
template <int S>
inline void unsplit(const uint8_t* src, uint8_t* o, uint32_t n) {
  for (int k = 0; k < S; ++k) {
    const uint8_t* s = src + (size_t)k * n;
    for (uint32_t i = 0; i < n; ++i) o[(size_t)i * S + k] = s[i];
  }
}

// ---------------------------------------------------------------- NA bitmap: set bit i -> out[i] = na (nw words)
template <class T>
inline void na_bitmap(const uint64_t* w, size_t nw, T* out, T na) {
  for (size_t k = 0; k < nw; ++k) {
    uint64_t m = w[k];
    while (m) {
      out[k * 64 + (size_t)__builtin_ctzll(m)] = na;
      m &= m - 1;
    }
  }
}

// ---------------------------------------------------------------- dictionary: out[i] = vals[codes[i]]
// false at the first code outside [0, nd) (out may then be partly written). out may alias codes (int32).
template <class T>
inline bool dict_gather(const int32_t* codes, uint32_t n, const T* vals, uint32_t nd, T* out) {
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t k = (uint32_t)codes[i];
    if (k >= nd) return false;
    out[i] = vals[k];
  }
  return true;
}

}  // namespace kern
}  // namespace fstx

#endif
