## Submission

This is a new submission.

## Test environments

* Windows 11, R 4.6.2 (Rtools): `R CMD check --as-cran`
* GitHub Actions: Windows (R release), macOS arm64 (R release), Ubuntu x86_64 (R 4.4, release, devel),
  Ubuntu aarch64 (R release)
* Ubuntu 24.04 (on fstx 1.0.0, whose R code and C++ code R uses are unchanged in 1.0.1): gcc 13 and clang 18;
  AddressSanitizer + UndefinedBehaviorSanitizer, ThreadSanitizer and valgrind runs of the test suite, all clean

## R CMD check results

0 errors | 0 warnings | 1 note

* checking compilation flags used ... NOTE
  Compilation used the following non-portable flag(s): '-Wa,-muse-unaligned-vector-move'

  This flag is added by `configure.win` on 64-bit Windows only. It works around GCC bug 54412: GCC does not
  align the stack to 32 bytes on Win64, so aligned AVX moves of spilled vector registers can crash. The flag
  makes the assembler emit unaligned moves instead. `configure.win` adds it only after checking that the
  compiler accepts it. Other platforms do not use it.
