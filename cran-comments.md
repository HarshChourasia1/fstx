## Submission

This is a new submission. It replaces fstx 1.0.2, which is still in the incoming queue (newbies); please drop
1.0.2 and review 1.2.0 instead. 1.0.2 contained the fixes asked for after 1.0.1 (the non-portable flag
'-Wa,-muse-unaligned-vector-move' removed from `configure.win`; DESCRIPTION wording). Both are kept in 1.2.0.

fstx 1.2.0 adds two column types, 64-bit integers ('bit64' integer64 vectors) and binary values (lists of raw
vectors), makes reads, filters and the Arrow stream faster, and fixes two bugs in `fstx_arrow_stream()` (see
NEWS.md).

* 'bit64' and 'blob' are new in Suggests. fstx does not call either package: an integer64 vector is a double vector
  with a class, and a blob is a list of raw vectors with a class. 'bit64' is only checked for with
  `requireNamespace()` to tell the user how to print an int64 column; the tests use both packages when installed.
* Files with the new column types need fstx 1.2 or later to read. fstx 1.2 reads every file fstx 1.0 wrote.

## Test environments

* Windows 11, R 4.6.2 (Rtools 4.5): `R CMD check --as-cran`
* Ubuntu 24.04 (WSL), R-devel (2026-11-14 r90890), gcc 13 with `-Wall -pedantic`: `R CMD check --as-cran`
* win-builder: R-devel and R-release
* Ubuntu 24.04, gcc 13: AddressSanitizer + UndefinedBehaviorSanitizer, ThreadSanitizer and valgrind (memcheck)
  runs of the full test suite
* GitHub Actions: Windows (R release), macOS arm64 (R release), Ubuntu x86_64 (R 4.4, release, devel),
  Ubuntu aarch64 (R release)

## R CMD check results

0 errors | 0 warnings | 1 note

* This is a new submission.

On the local R-devel check the only other NOTE was "Skipping checking math rendering: package 'V8' unavailable",
which is about that machine (V8 not installed), not the package. The sanitizer and valgrind runs report no errors
in fstx code.

## Reverse dependencies

There are no reverse dependencies.
