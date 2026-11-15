# fstx

Fast, safe column-store files for R data frames. Each column is split into pages; every page gets the
lightweight encoding that fits it (bit-packing, frame-of-reference, delta, run-length, dictionary, ALP for
doubles, FSST for strings), optionally under ZSTD or LZ4, with an XXH3 checksum per page. Reads and writes
run on all cores.

## Install

From R-universe (binary packages for Windows and macOS, source for Linux):

```r
install.packages("fstx", repos = c("https://GITHUB_USERNAME.r-universe.dev", "https://cloud.r-project.org"))
```

From source (needs Rtools on Windows, a C++17 compiler elsewhere), with the release tarball or a copy of
the source folder:

```
R CMD INSTALL fstx_0.7.0.tar.gz
R CMD INSTALL fstx
```

## Write and read

```r
library(fstx)
x <- data.frame(id = 1:1e6, city = sample(c("Pune", "Delhi", "Goa"), 1e6, TRUE), amount = runif(1e6))

write_fstx(x, "sales.fstx")                      # profile "balanced": encodings + ZSTD 3 where it pays
write_fstx(x, "sales_fast.fstx", profile = "fast")  # no codec: fastest reads
write_fstx(x[1:1000, ], "sales.fstx", append = TRUE)  # crash-safe append

y <- read_fstx("sales.fstx")
read_fstx("sales.fstx", columns = c("id", "amount"), from = 101, to = 200)
read_fstx("sales.fstx", rows = c(10, 5, 999999))        # only the pages that hold these rows
read_fstx("sales.fstx", where = city == "Goa" & amount > 0.99)
```

## Filters

`where` takes terms joined by `&`: comparisons with a value, `%in%`, `between()`, `is.na()`, `!is.na()`
or a logical column. Pages are skipped by their min / max / NA count before rows are tested.

```r
fstx_explain("sales.fstx", amount > 0.99, exact = TRUE)   # pages skipped, pages read, rows matched
fstx_which("sales.fstx", city == "Goa")                    # matching row numbers
```

## Strings, factors and lazy reads

- `strings = "auto"` (default): string pages are decoded in parallel and each character column comes
  back as an ALTREP vector whose R strings are made a page at a time on first use. Columns with few
  distinct values (all pages dictionary-coded) come back as plain character vectors. `saveRDS()` writes
  the standard format.
- `strings = "eager"`: plain character vectors, all strings made during the read.
- `strings = "deferred"`: like auto for every string column, with a compact serialised form.
- `as_factor = TRUE`: dictionary-coded columns become factors without hashing the rows.
- `lazy = TRUE`: returns at once; columns are decoded from the memory-mapped file when used.

## Inspect and maintain

```r
fstx_meta("sales.fstx")      # rows, columns, sizes, profile (footer only)
fstx_pages("sales.fstx")     # one row per page: encoding, codec, sizes
fstx_compact("sales.fstx")   # drop what appends leave behind; merge = TRUE rewrites row groups
fstx_simd()                  # SIMD target in use (SSE2 / SSE4 / AVX2, chosen at run time)
```

See `?read_fstx`, `?write_fstx` and `?fstx` for every argument. Third-party code bundled under
`src/vendor/` and its licences are listed in `inst/COPYRIGHTS`.
