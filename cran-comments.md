<!-- DRAFT for fstx 1.2.0, written before the 1.2.0 check runs. Before submitting: fill in "Test environments"
and "R CMD check results" from those runs, set the first line of "Submission" to how CRAN then lists fstx (an
update of 1.0.2 once that is on CRAN), and delete this comment. Nothing below the comment has been run on 1.2.0. -->

## Submission

fstx 1.2.0 follows fstx 1.0.2. It adds two column types, 64-bit integers ('bit64' integer64 vectors) and binary
values (lists of raw vectors), makes reads, filters and the Arrow stream faster, and fixes two bugs in
`fstx_arrow_stream()` (see NEWS.md).

* 'bit64' and 'blob' are new in Suggests. fstx does not call either package: an integer64 vector is a double vector
  with a class, and a blob is a list of raw vectors with a class. 'bit64' is only checked for with
  `requireNamespace()` to tell the user how to print an int64 column; the tests use both packages when installed.
* Files with the new column types need fstx 1.2 or later to read. fstx 1.2 reads every file fstx 1.0 wrote.

## Test environments

PENDING: the planned runs; keep only those that ran and passed.

* Windows 11, R 4.6.2 (Rtools 4.5): `R CMD check --as-cran`
* Ubuntu 24.04 (WSL), R-devel: `R CMD check --as-cran`
* Ubuntu 24.04, gcc 13 and clang 18: AddressSanitizer + UndefinedBehaviorSanitizer, ThreadSanitizer and valgrind
  runs of the test suite
* GitHub Actions: Windows (R release), macOS arm64 (R release), Ubuntu x86_64 (R 4.4, release, devel),
  Ubuntu aarch64 (R release)

## R CMD check results

PENDING: from the 1.2.0 runs above.

## Reverse dependencies

There are no reverse dependencies.
