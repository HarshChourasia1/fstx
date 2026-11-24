## Resubmission

This is a resubmission of fstx (1.0.1 was not accepted by the incoming pre-tests on 23 Nov 2026). In 1.0.2:

* The non-portable flag '-Wa,-muse-unaligned-vector-move' is gone. `configure.win` no longer adds it; with GCC
  on 64-bit Windows it leaves the AVX2 code paths out instead (GCC bug 54412), so the package builds with
  portable flags only.
* DESCRIPTION: the words flagged as possibly misspelled ('SIMD', 'fstx') are reworded or quoted.

This is a new submission.

## Test environments

* Windows 11, R 4.6.2 (Rtools 4.5): `R CMD check --as-cran`
* GitHub Actions: Windows (R release), macOS arm64 (R release), Ubuntu x86_64 (R 4.4, release, devel),
  Ubuntu aarch64 (R release)
* Ubuntu 24.04 (on fstx 1.0.0, whose R code and C++ code R uses are unchanged apart from the Windows build
  setting above): gcc 13 and clang 18; AddressSanitizer + UndefinedBehaviorSanitizer, ThreadSanitizer and
  valgrind runs of the test suite, all clean

## R CMD check results

0 errors | 0 warnings | 1 note

* This is a new submission.
