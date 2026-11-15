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
