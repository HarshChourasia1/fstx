# fstx 1.2.0

## New column types

* 64-bit integers. `bit64::integer64` columns are stored as int64 columns: exact values, missing values kept,
  integer encodings (frame-of-reference, delta, dictionary and the rest) and page statistics like other integer
  columns. `write_fstx(int64 = "native")` is the default; `int64 = "integer64"` writes the fstx 1.0 layout instead
  (the bits in a double column with class "integer64"), for files that fstx 1.0 must read.
* `read_fstx(int64 = "integer64")` (default) returns int64 columns as integer64; `int64 = "double"` returns doubles
  and warns once when a value past 2^53 loses precision. Columns in the 1.0 layout read as before.
* bit64 is suggested, not imported: without it, int64 columns still read as correct integer64 vectors, with one
  message per column and session.
* `where`, `fstx_which()` and `fstx_explain()` filter int64 columns exactly (`==`, `!=`, `<`, `<=`, `>`, `>=`,
  `%in%`, `between()`, `is.na()`), with integer, double or integer64 values. Columns in the 1.0 layout still
  cannot be filtered.
* Binary columns. A list column whose elements are raw vectors or `NULL` (`NULL` is missing) is stored as a binary
  column; no class, `"AsIs"`, `"blob"` (blob package), `"arrow_binary"` and vctrs `list_of` of raw are accepted.
  Values may hold any bytes, NUL included; they are read back as a list of raw vectors with their class, so blob
  objects round-trip without blob being needed. Binary columns use the same page types as strings (plain, FSST,
  dictionary). In `where` they take only `is.na()` and `!is.na()`.
* `lazy = TRUE` reads int64 columns lazily like doubles; binary columns are read at once. `strings` and
  `as_factor` do not apply to binary columns.
* `fstx_arrow_stream()` passes int64 columns as Arrow int64 and binary columns as Arrow binary.
* `fstx_meta()`, `fstx_pages()` and `fstx_features()` show the types "int64" and "binary" and the page kind "int64".

## File format

* fstx format 1.2, still format version 3 in the file. Its specification ships with the package:
  `system.file("spec", "fstx-format-1.2.md", package = "fstx")`. The 1.0 specification stays alongside it.
* A file with an int64 or binary column sets the required feature bit 16 (`int64`) or 17 (`binary`). fstx 1.0.x
  refuses such a file with a message that names the feature and ends "Update fstx". Every other file fstx 1.2
  writes is one that fstx 1.0 reads with the same result.
* Appends follow the column types already in the file (a 1.0-layout integer64 column gets 1.0-layout rows), so an
  append never changes the feature bits. `fstx_compact(merge = TRUE)` keeps each column's stored type.
* Files record "fstx 1.2.0" as the writer.

## Speed

* Filters (`where`) and row lists (`rows`) read their pages with positional reads instead of faulting in the
  memory map (page faults are serialised per process on Windows), decode only the strings at the rows asked for,
  and keep their buffers between reads.
* `fstx_arrow_stream()`: row ranges are read in parallel, string pages are decoded straight into the Arrow buffers
  page by page in parallel, and filtered streams use the same plan as `where`.
* FSST pages decode in one loop over their strings.
* The writer keeps at most 8 MB of string pointers for a column (was 64 MB): less memory when writing large
  character columns, same files.
* Benchmark of 27 Nov 2026 (one Windows desktop, 14 threads, warm cache, tables of 2 to 10 million rows; the
  development build before the new column types): `read_fstx()` with a filter took 0.0145 to 0.0188 s against
  0.251 to 0.339 s for arrow's Parquet reader, and a full read 0.0413 to 0.0632 s against 0.364 to 0.496 s.

## Bug fixes

* `fstx_arrow_stream()` failed with "dictionary and plain pages mixed" on a string column whose row groups were
  partly dictionary-coded and partly plain (for example a short last row group). `read_fstx()` was not affected.
* `fstx_arrow_stream()` with `where =` could crash in Windows builds when the stream was read from a thread other
  than R's (codec state kept per thread in the GCC build).
* The writer's choice between plain and FSST string pages under a codec now compares the two as pages of their
  real size. It took the larger encoding for some text columns (one benchmark column: 44.78 MB, now 44.22 MB).

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
