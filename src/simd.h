// FST-X SIMD kernels (M4): Highway with dynamic dispatch (simd_kernels.cpp), one entry per hot loop. Each gives
// bit-identical results to its plain version in kern_scalar.h. The kx:: wrappers pick SIMD or plain at run
// time (simd::on(); the R option / FSTX_SIMD variable sets it). No R headers. MIT licence.
#ifndef FSTX_SIMD_H
#define FSTX_SIMD_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "kern_scalar.h"

namespace fstx {
namespace simd {

extern std::atomic<bool> g_on;
inline bool on() { return g_on.load(std::memory_order_relaxed); }

// targets as Highway bit masks
int64_t compiled_targets();   // compiled into this build
int64_t cpu_targets();        // compiled and supported by this CPU
int64_t active_target();      // the one the kernels use now (0 when SIMD is off)
const char* target_name(int64_t t);
int64_t dispatched_target();  // the target of the kernel copy the dispatcher calls now
// mode: 0 = off (plain loops), 1 = best target, 2 = only the targets in mask (best of them that the CPU runs).
// Returns false (and changes nothing) when mode 2 leaves no target the CPU can run.
bool set_mode(int mode, int64_t mask);

void unpack32(int w, const uint32_t* in, uint32_t* out, uint32_t base);
void unpack64(int w, const uint64_t* in, uint64_t* out, uint64_t base);
void pack32(int w, const uint32_t* in, uint32_t* out);
void pack64(int w, const uint64_t* in, uint64_t* out);
void delta32(uint32_t* o, uint32_t m, uint32_t first);
void delta64(uint64_t* o, uint32_t m, uint64_t first);
void minmax32(const int32_t* s, uint32_t m, int32_t& lo, int32_t& hi);
void minmax64(const int64_t* s, uint32_t m, int64_t& lo, int64_t& hi);
void alp_decode(double* out, uint32_t n, int e, int f);
void unsplit4(const uint8_t* src, uint8_t* o, uint32_t n);
void unsplit8(const uint8_t* src, uint8_t* o, uint32_t n);
void na_bitmap32(const uint64_t* w, size_t nw, int32_t* out, int32_t na);
void na_bitmap64(const uint64_t* w, size_t nw, uint64_t* out, uint64_t na);
bool dict32(const int32_t* codes, uint32_t n, const int32_t* vals, uint32_t nd, int32_t* out);
bool dict64(const int32_t* codes, uint32_t n, const int64_t* vals, uint32_t nd, int64_t* out);

}  // namespace simd

// ---------------------------------------------------------------- run-time choice, SIMD or plain
namespace kx {

template <class U>
inline void unpack(int w, const U* in, U* out, U base) {
  if (simd::on()) {
    if constexpr (sizeof(U) == 4) simd::unpack32(w, (const uint32_t*)in, (uint32_t*)out, (uint32_t)base);
    else simd::unpack64(w, (const uint64_t*)in, (uint64_t*)out, (uint64_t)base);
  } else {
    kern::unpack<U>(w, in, out, base);
  }
}
template <class U>
inline void pack(int w, const U* in, U* out) {
  if (simd::on()) {
    if constexpr (sizeof(U) == 4) simd::pack32(w, (const uint32_t*)in, (uint32_t*)out);
    else simd::pack64(w, (const uint64_t*)in, (uint64_t*)out);
  } else {
    kern::pack<U>(w, in, out);
  }
}
template <class U>
inline void delta_sum(U* o, uint32_t m, U first) {
  if (simd::on()) {
    if constexpr (sizeof(U) == 4) simd::delta32((uint32_t*)o, m, (uint32_t)first);
    else simd::delta64((uint64_t*)o, m, (uint64_t)first);
  } else {
    kern::delta_sum<U>(o, m, first);
  }
}
template <class T>
inline void minmax(const T* s, uint32_t m, T& lo, T& hi) {
  if (simd::on()) {
    if constexpr (sizeof(T) == 4) simd::minmax32((const int32_t*)s, m, (int32_t&)lo, (int32_t&)hi);
    else simd::minmax64((const int64_t*)s, m, (int64_t&)lo, (int64_t&)hi);
  } else {
    kern::minmax<T>(s, m, lo, hi);
  }
}
inline void alp_decode(double* out, uint32_t n, int e, int f) {
  if (simd::on()) simd::alp_decode(out, n, e, f);
  else kern::alp_decode(out, n, e, f);
}
template <int S>
inline void unsplit(const uint8_t* src, uint8_t* o, uint32_t n) {
  if (simd::on() && S == 4) simd::unsplit4(src, o, n);
  else if (simd::on() && S == 8) simd::unsplit8(src, o, n);
  else kern::unsplit<S>(src, o, n);
}
template <class T>
inline void na_bitmap(const uint64_t* w, size_t nw, T* out, T na) {
  static_assert(sizeof(T) == 4 || sizeof(T) == 8, "NA bitmap: 4 or 8 byte values");
  if (simd::on()) {
    if constexpr (sizeof(T) == 4) {
      int32_t v;
      std::memcpy(&v, &na, 4);
      simd::na_bitmap32(w, nw, (int32_t*)out, v);
    } else {
      uint64_t v;
      std::memcpy(&v, &na, 8);
      simd::na_bitmap64(w, nw, (uint64_t*)out, v);
    }
  } else {
    kern::na_bitmap<T>(w, nw, out, na);
  }
}
template <class T>
inline bool dict_gather(const int32_t* codes, uint32_t n, const T* vals, uint32_t nd, T* out) {
  if (simd::on()) {
    if constexpr (sizeof(T) == 4) return simd::dict32(codes, n, (const int32_t*)vals, nd, (int32_t*)out);
    else return simd::dict64(codes, n, (const int64_t*)vals, nd, (int64_t*)out);
  }
  return kern::dict_gather<T>(codes, n, vals, nd, out);
}

}  // namespace kx
}  // namespace fstx

#endif
