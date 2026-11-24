# fstx 1.0.2

* Windows builds made with GCC (Rtools, as on CRAN) no longer use the assembler flag
  `-Wa,-muse-unaligned-vector-move`. They leave out the AVX2 kernels instead (GCC bug 54412: GCC cannot align the
  Win64 stack for 32-byte vectors) and decode with SSE4. Files are unchanged; other platforms are unchanged.

# fstx 1.0.1

* No change for R users: files, results and speed are as in 1.0.0.
* The engine now also builds with Microsoft's C++ compiler (used for the Python package, `pip install fstx`), and
  the Arrow stream can pass Date and POSIXct columns as stored numbers (used by Python only; R is unchanged).
* README: install from R-universe.

# fstx 1.0.0

* First public release. Needs R 4.4.0 or newer.
* Reads and writes data frames in the fstx column-store format: per-page lightweight encodings
  (bit-packing, frame-of-reference, delta, run-length, dictionary, ALP, FSST) under optional LZ4 or
  Zstandard compression, a page directory with per-page statistics, and a checksum on every page.
* Parallel reads and writes with SIMD decode kernels chosen at run time; lazy ALTREP columns over the
  memory-mapped file; character columns whose R strings are made on first use; factors from string
  dictionaries.
* Partial reads by column, row window or row list; filters that skip pages by their statistics;
  crash-safe appends and compaction.
* `fstx_arrow_stream()` reads a file as an Arrow C stream (through 'nanoarrow') without making R strings.
  Numeric, logical, factor and date-time columns are split into row parts across threads when a batch
  has fewer columns than threads.
* Column and table attributes that are plain vectors (class, row names, data.table key and index, labels) are
  stored in fstx's own checked encoding (spec section 9.4), so ordinary files are read without
  `unserialize()`; other attributes still use `serialize()`, and `read_fstx(attributes = FALSE)` skips them.
  Reads and appends check table attributes; appending rows to a data.table drops its key and indices.
* `options(fstx.verify_nulls = TRUE)` (off by default) checks each page's stored missing-value count
  against its decoded values, in reads, filters and the Arrow stream, and refuses a page that disagrees
  as corrupt.
* Writing a file that is open in this R session is refused, also through another spelling of its path;
  on Windows the check uses the 128-bit file id (ReFS) and falls back to the 64-bit file index.
* The default thread count is at most 2 under R CMD check core limits (`_R_CHECK_LIMIT_CORES_`) and
  at most `OMP_THREAD_LIMIT` when that is set.

## File format

* fstx format 1.0, stored as format version 3 in the file. The format is written down in a specification
  that ships with the package: `system.file("spec", "fstx-format-1.0.md", package = "fstx")`.
* A file carries feature flags. A file that needs a feature this fstx does not know (written by a later
  fstx) is refused with a message that names the feature and ends "Update fstx", instead of being called
  corrupt. Numbers reserved for later versions (new column types, codecs, encodings) get the same kind of
  message.
* A file with an optional feature this fstx does not know reads normally, but appending to it or
  compacting it with page copy is refused: "Update fstx to change this file; reading works."
* `fstx_compact(merge = TRUE)` reads the whole table and writes a fresh file. Optional features this fstx
  does not know are dropped, and a profile written by a later fstx is replaced by "balanced".
* `fstx_meta()$profile` is "unknown" for a profile that a later fstx wrote. The file reads normally.
* Each file records the fstx version that wrote it.
* Files from fstx 0.x development versions (format versions 1 and 2) are refused with a message that says
  so.
* `fstx_features()` lists what this build can read and write: format versions, file feature names, codecs
  and column types.
