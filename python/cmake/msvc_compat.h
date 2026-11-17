/* fstx, Python build with Microsoft cl.exe only (not clang-cl, not GCC): force-included ahead of every
 * engine and vendor source (/FI, see ../CMakeLists.txt) so that the engine sources need no cl-specific
 * edits for the GCC / Clang spellings they use:
 *   __builtin_ctzll, __builtin_clzll   bit scans (engine and vendor/fsst)
 *   __attribute__((...))               always_inline and printf-format hints: dropped (hints only)
 * The one thing this cannot cover is the GCC vector extension (vector_size); those few functions have a
 * plain-loop version in the engine itself under FSTX_NO_VECTOR_EXT. MIT licence. */
#ifndef FSTX_MSVC_COMPAT_H
#define FSTX_MSVC_COMPAT_H

#if defined(_MSC_VER) && !defined(__clang__)

#include <intrin.h>

#ifdef __cplusplus
#define FSTX_COMPAT_INLINE inline
#else
#define FSTX_COMPAT_INLINE static __inline
#endif

/* Both are undefined for 0 in GCC; callers never pass 0. */
FSTX_COMPAT_INLINE int fstx_compat_ctzll(unsigned long long x) {
  unsigned long i;
#if defined(_M_X64) || defined(_M_ARM64)
  _BitScanForward64(&i, x);
#else
  if (_BitScanForward(&i, (unsigned long)x)) return (int)i;
  _BitScanForward(&i, (unsigned long)(x >> 32));
  i += 32;
#endif
  return (int)i;
}
FSTX_COMPAT_INLINE int fstx_compat_clzll(unsigned long long x) {
  unsigned long i;
#if defined(_M_X64) || defined(_M_ARM64)
  _BitScanReverse64(&i, x);
#else
  if (_BitScanReverse(&i, (unsigned long)(x >> 32))) return 31 - (int)i;
  _BitScanReverse(&i, (unsigned long)x);
  return 63 - (int)i;
#endif
  return 63 - (int)i;
}

#define __builtin_ctzll(x) fstx_compat_ctzll(x)
#define __builtin_clzll(x) fstx_compat_clzll(x)
#define __attribute__(x)

#endif /* cl.exe */

#endif
