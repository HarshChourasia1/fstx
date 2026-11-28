# fstx

[![PyPI](https://img.shields.io/pypi/v/fstx)](https://pypi.org/project/fstx/)
[![CRAN](https://www.r-pkg.org/badges/version/fstx)](https://cran.r-project.org/package=fstx)
[![R-universe](https://harshchourasia1.r-universe.dev/badges/fstx)](https://harshchourasia1.r-universe.dev/fstx)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

Fast, safe column-store files for Python and R. One C++ engine sits under both packages, so a file written
from Python reads in R and the other way round. Each column is split into pages; every page gets the
lightweight encoding that fits it (bit-packing, frame-of-reference, delta, run-length, dictionary, ALP for
doubles, FSST for strings), optionally under ZSTD or LZ4, with an XXH3 checksum per page. Reads and writes
run on all cores.

Docs: [https://harshchourasia1.github.io/fstx/](https://harshchourasia1.github.io/fstx/)

![fstx vs Vortex vs Parquet: time in milliseconds](https://raw.githubusercontent.com/HarshChourasia1/fstx/main/.github/readme/speed.svg)

## Install

Python 3.10 or newer; wheels for Windows x64, Linux x86_64 and aarch64, and macOS (Intel and Apple silicon):

```
pip install fstx
```

R 4.4.0 or newer; ready-built for Windows and macOS (and from source on Linux) through R-universe:

```r
install.packages("fstx", repos = c("https://harshchourasia1.r-universe.dev", "https://cloud.r-project.org"))
```

R from source (needs a C++17 compiler; Rtools on Windows): `remotes::install_github("HarshChourasia1/fstx")`.

## Quick start

Python (tables are `pyarrow.Table`s; pandas and polars helpers on top):

```python
import fstx

fstx.write_fstx(table, "sales.fstx")                  # a pyarrow Table, or a pandas / polars DataFrame
t = fstx.read_fstx("sales.fstx", columns=["city", "price"])
t = fstx.read_fstx("sales.fstx", filters=[("price", ">", 500), ("city", "in", ["Pune", "Mumbai"])])
df = fstx.to_pandas(t)
```

R:

```r
library(fstx)
write_fstx(x, "sales.fstx")
y <- read_fstx("sales.fstx", where = city == "Goa" & amount > 0.99)
```

The Python guide (every option, pandas and polars, how each type maps, speed against Parquet, Vortex, Lance and
DuckDB) is [python/README.md](https://github.com/HarshChourasia1/fstx/blob/main/python/README.md). The R guide
follows.

## Speed

Python, all cores, seconds; one desktop (Intel Core Ultra 5 245K, 14 cores, NVMe SSD, Windows x64), 27 Nov 2026,
the 1.2 speed work before the new column types. Each case runs in a fresh process; a time is the median of 3 runs,
the better of 2 rounds; page cache warm. Tables: numeric 10,000,000 rows x 6, text 2,000,000 x 4 strings, mixed
5,000,000 x 8 (numbers, a date, strings, a factor, a logical). Also run: Parquet zstd, Lance, DuckDB and Polars;
the "fastest" column covers all of them.

| table | test | fstx | Vortex | Parquet (snappy) | fastest |
|---|---|---:|---:|---:|---|
| numeric | write | **0.0738** | 0.233 | 0.914 | fstx |
| numeric | full read | **0.0227** | 0.0400 | 0.0802 | fstx |
| numeric | one column | **0.0117** | 0.0207 | 0.0441 | fstx |
| numeric | 1% row window | **0.00124** | 0.00276 | 0.0138 | fstx |
| numeric | filter | **0.0209** | 0.0490 | 0.0803 | fstx |
| text | write | **0.104** | 0.330 | 0.557 | fstx |
| text | full read | **0.0394** | 0.0438 | 0.0882 | fstx |
| text | one column | 0.0190 | **0.0189** | 0.0874 | Vortex |
| text | 1% row window | **0.00152** | 0.00279 | 0.0891 | fstx |
| text | filter | **0.0115** | 0.0338 | 0.0829 | fstx |
| mixed | write | **0.0954** | 0.346 | 0.869 | fstx |
| mixed | full read | **0.0322** | 0.0422 | 0.133 | fstx |
| mixed | one column | 0.00613 | **0.00528** | 0.0349 | Vortex |
| mixed | 1% row window | 0.00387 | **0.00319** | 0.0430 | Vortex |
| mixed | filter | **0.0173** | 0.0306 | 0.121 | fstx |
| | file size, MB (numeric / text / mixed) | **119 / 70.8 / 64.8** | 120 / 101 / 83.7 | 204 / 117 / 128 | |

Counting the one-thread runs too, fstx was the fastest in 26 of 30 cases; Vortex was faster in four: the three
above and the mixed row window at one thread. Reads of a few milliseconds are noisy. More detail, memory use included, is in
the [Python guide](https://github.com/HarshChourasia1/fstx/blob/main/python/README.md#speed).

## R: write and read

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

## R: filters

`where` takes terms joined by `&`: comparisons with a value, `%in%`, `between()`, `is.na()`, `!is.na()`
or a logical column. Pages are skipped by their min / max / NA count before rows are tested.

```r
fstx_explain("sales.fstx", amount > 0.99, exact = TRUE)   # pages skipped, pages read, rows matched
fstx_which("sales.fstx", city == "Goa")                    # matching row numbers
```

## R: strings, factors and lazy reads

- `strings = "auto"` (default): string pages are decoded in parallel and each character column comes
  back as an ALTREP vector whose R strings are made a page at a time on first use. Columns with few
  distinct values (all pages dictionary-coded) come back as plain character vectors. `saveRDS()` writes
  the standard format.
- `strings = "eager"`: plain character vectors, all strings made during the read.
- `strings = "deferred"`: like auto for every string column, with a compact serialised form.
- `as_factor = TRUE`: dictionary-coded columns become factors without hashing the rows.
- `lazy = TRUE`: returns at once; columns are decoded from the memory-mapped file when used.

## R: inspect and maintain

```r
fstx_meta("sales.fstx")      # rows, columns, sizes, profile (footer only)
fstx_pages("sales.fstx")     # one row per page: encoding, codec, sizes
fstx_compact("sales.fstx")   # drop what appends leave behind; merge = TRUE rewrites row groups
fstx_simd()                  # SIMD target in use (SSE2 / SSE4 / AVX2, chosen at run time)
```

## R: column types

How Python types map is in the Python guide.

| R | stored as | notes |
|---|---|---|
| integer, double, logical, character | int32, float64, logical, string | NA kept; NaN, Inf and -0 kept in doubles |
| factor, ordered | int32 codes with the levels | |
| Date | int32 or float64 days | as R holds it |
| POSIXct | float64 seconds with the time zone | the double R holds; nanoseconds are not kept |
| `bit64::integer64` | int64 (fstx 1.2) | `write_fstx(int64 = "integer64")` writes the fstx 1.0 layout; `read_fstx(int64 = "double")` reads doubles |
| list of raw vectors (also `blob`, `arrow_binary`, `I()`) | binary (fstx 1.2) | `NULL` is missing; any bytes, NUL included |
| other attributes (labels, units, classes) | an attribute blob per column | |

A file with an int64 or binary column needs fstx 1.2 or later to read. `where` filters
int64 columns exactly; binary columns take only `is.na()`.

## File format

fstx 1.2 writes fstx format 1.2 (format version 3 inside the file): format 1.0 plus the int64 and binary column
types, the same from Python and R. Every later fstx reads these files, and fstx 1.2 reads every file fstx 1.0
wrote. The specification is [inst/spec/fstx-format-1.2.md](https://github.com/HarshChourasia1/fstx/blob/main/inst/spec/fstx-format-1.2.md)
(in R also `system.file("spec", "fstx-format-1.2.md", package = "fstx")`; `fstx-format-1.0.md` is what fstx 1.0
implements).

- A file from a later fstx that needs something this version does not know is refused with a message that
  names it and ends "Update fstx". It is not called corrupt.
- `fstx_features()` (R) lists what this build reads and writes.
- R: column and table attributes that are plain vectors (class, row names, data.table key and index, labels)
  are stored in fstx's own checked encoding. Other attributes (lists, complex values; and all attributes in
  files from earlier fstx 1.0 builds) use R's `serialize()` and are restored with `unserialize()`, as
  `readRDS()` does: only read such files from sources you trust, or use `read_fstx(attributes = FALSE)`, which
  skips them. The checksum catches accidental damage, not deliberate tampering.

In R, see `?fstx` for a short guide, and `?read_fstx` and `?write_fstx` for every argument.

## Credits

fstx's ALP and ALP-RD double encodings follow Afroozeh, Kuffo and Boncz, "ALP: Adaptive Lossless floating-Point
Compression", Proc. ACM Manag. Data 1(4), 2023, [doi:10.1145/3626717](https://doi.org/10.1145/3626717). String
pages use FSST (Boncz, Neumann and Leis, "FSST: Fast Random Access String Compression", PVLDB 13(12), 2020,
[doi:10.14778/3407790.3407851](https://doi.org/10.14778/3407790.3407851)), bundled from cwida/fsst. fstx also
bundles Zstandard, LZ4, xxHash, FlatBuffers and Highway, and the Apache Arrow C Data Interface structs; each with
its licence, listed in [inst/COPYRIGHTS](inst/COPYRIGHTS) (the Python package ships the same list in
`python/LICENSES/`).

## Citing fstx

GitHub's "Cite this repository" button gives APA and BibTeX from [CITATION.cff](https://github.com/HarshChourasia1/fstx/blob/main/CITATION.cff); in R,
`citation("fstx")`.
