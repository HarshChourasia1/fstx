# fstx 1.0.0

## Requirements

* fstx now needs R 4.4.0 or newer.

## File format

* New file format: fstx format 1.0, stored as format version 3 in the file. Every new file is written in it.
  The format is written down in a specification that ships with the package:
  `system.file("spec", "fstx-format-1.0.md", package = "fstx")`.
* Files written by fstx 0.7 (format 2) are read as they are. Appending to one, or compacting it with
  `fstx_compact()` (the default page copy), keeps it in format 2, so fstx 0.7 can still read it.
* fstx 0.7 cannot read format 3 files. It stops with a message that ends "Update fstx".
* A format 3 file carries feature flags. A file that needs a feature this fstx does not know (written by a
  later fstx) is refused with a message that names the feature and ends "Update fstx", instead of being
  called corrupt. Numbers reserved for later versions (new column types, codecs, encodings) get the same
  kind of message.
* A file with an optional feature this fstx does not know reads normally, but appending to it or
  compacting it with page copy is refused: "Update fstx to change this file; reading works."
* `fstx_compact(merge = TRUE)` reads the whole table and writes a fresh file in the current format. A
  format 2 (fstx 0.7) file becomes format 3, so fstx 0.7 can no longer read it. Optional features this fstx
  does not know are dropped, and a profile written by a later fstx is replaced by "balanced". Use the
  default `merge = FALSE` to keep a 0.7 file readable by 0.7.
* `fstx_meta()$profile` is "unknown" for a profile that a later fstx wrote. The file reads normally.
* Each file records the fstx version that wrote it.
* Format 1 files (an early prototype) are still refused; the message now says this version reads formats 2
  and 3.

## New functions

* `fstx_features()` lists what this build can read and write: format versions, file feature names, codecs
  and column types.
* `fstx_upgrade(path, dest = NULL)` rewrites a file written by fstx 0.7 in the current format; `dest` is
  the output file (`NULL` replaces the file).

## API changes (the R API is fixed from 1.0.0)

* `write_fstx()` no longer has the test-only arguments `.encoding` and `.crash`. They were never meant for
  users; a call that passes them now gets R's "unused argument" error.
* `read_fstx(strings = "altrep")` is now called `strings = "deferred"`. The old name still works through
  fstx 1.x but gives a warning.
* `verify` in `read_fstx()`, `exact` in `fstx_explain()` and `merge` in `fstx_compact()` must be `TRUE` or
  `FALSE`. Before, any other value (such as `1` or `NA`) quietly counted as `FALSE`, so `verify = 1` turned
  checksum checks off.
* Errors about arguments now start with the name of the function, for example
  `read_fstx: 'threads' must be a single whole number >= 1`. `fstx_simd()` gives a plain message for a bad
  `target`.
* `write_fstx(append = TRUE)` returns `x` as it was passed in. Before, an append that added factor levels
  returned the rows re-coded to the file's levels.
* Help pages, the messages of the R functions and the `fstx_meta()` print-out say "fstx" instead of
  "FST-X". The print-out now
  starts with "fstx file:".

# fstx 0.7.0

* First public release.
* Reads and writes data frames in the FST-X column-store format: per-page lightweight encodings
  (bit-packing, frame-of-reference, delta, run-length, dictionary, ALP, FSST) under optional LZ4 or
  Zstandard compression, a page directory with per-page statistics, and a checksum on every page.
* Parallel reads and writes with SIMD decode kernels chosen at run time; lazy ALTREP columns over the
  memory-mapped file; character columns whose R strings are made on first use; factors from string
  dictionaries.
* Partial reads by column, row window or row list; filters that skip pages by their statistics;
  crash-safe appends and compaction.
* The default thread count is at most 2 under R CMD check core limits (`_R_CHECK_LIMIT_CORES_`) and
  at most `OMP_THREAD_LIMIT` when that is set.
