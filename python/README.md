# fstx for Python

Reads and writes fstx files (the FST-X column-store format of the R package fstx 1.0) from Python, on the same
C++ engine as the R package. A file written by either language is an ordinary fstx 1.0 file that the other reads.
Tables are `pyarrow.Table`s; pandas and polars are helpers on top. Python 3.10+, pyarrow 15+; pandas, polars and
numpy are optional. The design, type by type, is in `DESIGN.md`; build and test notes are in `DEV.md`.

## Usage

```python
import fstx

# read: a pyarrow.Table
t = fstx.read_fstx("sales.fstx")
t = fstx.read_fstx("sales.fstx", columns=["city", "price"])           # names, or 0-based positions
t = fstx.read_fstx("sales.fstx", rows=slice(0, 1000))                  # 0-based, half-open; or a list of rows
t = fstx.read_fstx("sales.fstx", filters=[("price", ">", 500), ("city", "in", ["Pune", "Mumbai"])])

df = fstx.to_pandas(t)        # nullable dtypes, row names as the index, R's metadata in df.attrs["fstx"]
pl_df = fstx.to_polars(t)     # values only: polars has no metadata and no index

# write: a pyarrow.Table, RecordBatch or RecordBatchReader, a pandas or polars DataFrame
fstx.write_fstx(t, "out.fstx")                        # profile "balanced": Zstandard 3 where it saves 10%
fstx.write_fstx(df, "out.fstx", profile="small")      # "fast" (no codec), "balanced", "small" (Zstandard 9)
fstx.write_fstx(more_rows, "out.fstx", append=True)   # same columns and types; factor levels may grow

# the file without reading it
m = fstx.metadata("sales.fstx")       # nrow, ncol, columns (type, levels, time zone, R class), schema, ...
with fstx.open("sales.fstx") as f:    # the open file: f.schema, f.read(...), f.to_batches(...)
    for batch in f.to_batches(batch_rows=100_000):
        ...
```

Options of `read_fstx`: `threads=None` (all cores), `verify=True` (checksums), `as_factor=False` (strings as
dictionaries), `attributes=True`, `datetimes="arrow"` (`date32` / `timestamp[us]`; `"float"` gives the stored
numbers exactly), `invalid_utf8="replace"` (`"error"`, `"binary"`). Options of `write_fstx`: `profile`,
`compress` (`"zstd"`, `"lz4"`, `"none"`), `level`, `threads`, `append`, `int64` (`"auto"`: int32 when the
values fit, else bit64 integer64; `"integer64"`; `"double"`).

How R's types arrive:

| R | Python (pyarrow) | pandas (`to_pandas`) |
|---|---|---|
| logical, integer, double, character | `bool`, `int32`, `double`, `string` | `boolean`, `Int32`, `Float64`, `str` |
| factor (ordered) | `dictionary<int32, string>` (ordered) | `category` |
| Date | `date32` | `datetime.date` objects |
| POSIXct with tzone | `timestamp[us, tz]` | `datetime64[us, tz]` |
| bit64 integer64 | `int64` | `Int64` |
| difftime, labels, other attributes | the values, with the attributes in the field metadata | the same, in `df.attrs["fstx"]` |

R's NA is a null everywhere; NaN stays NaN through pyarrow and polars (pandas 3 makes it missing). A table read
here and written back keeps how R held it (classes, time zones, integer vs double Dates, row names): write from
the pyarrow.Table or from `fstx.to_pandas()`. The few things that do not survive are listed in `DESIGN.md`
section 8.

Rows are 0-based in Python (R's are 1-based). Filters are AND-ed terms `(column, op, value)` with ops
`== != < <= > >= in is_null not_null`, as R's `where =`.

## Speed

Measured 18 Nov 2026 on one desktop: Intel Core Ultra 5 245K (14 cores), 31.4 GiB RAM, Samsung 990 EVO Plus
NVMe SSD, Windows x64; Python 3.12.10, pyarrow 26.0.1, pandas 3.1.1, this package as the wheel built with
MSVC 19.44; R 4.6.1 with fstx 1.0.0. One table of 10,000,000 rows and 10 columns (two int32, two double, a
logical, a 20-value string, a string unique per row, a date, a time with a time zone, a 5-level factor;
692 MiB in Arrow memory). Median of 5 runs, page cache warm, all cores unless stated. Scripts:
`tools/bench_py.py` and `tools/bench_r.R`.

| seconds | fstx (balanced) | Parquet (snappy) | Feather (lz4) | Feather (none) | R fstx |
|---|---|---|---|---|---|
| write | 0.29 (was 0.44) | 2.18 | 0.47 | 0.23 | 0.15 |
| write, 1 thread | 1.20 (was 1.36) | 2.17 | - | - | 0.86 |
| read | 0.24 (was 0.24) | 0.20 | 0.20 | 0.10 | 2.48 (0.09 deferred) |
| read, 1 thread | 0.60 (was 0.69) | 0.42 | 0.18 | - | 2.83 |
| read 2 columns (a double, the 20-value string) | 0.09 (was 0.19) | 0.10 | 0.27 | - | 0.04 |
| read into pandas | 0.57 (was 0.49) | 0.41 | - | - | - |
| file size, MiB | 222.8 (was 224) | 372 | 490 | 693 | 222.8 |

"was": the first measurement (same machine and table, earlier the same day), before the Python writer handed
strings that repeat as a dictionary (equal strings must reach the engine as one pointer, as R gives them, to be
dictionary-coded; DESIGN.md section 6). Since then the 20-value string column is stored as R stores it, the
file is the size of R's, and reading that column is about twice as fast. The string column unique per row
also writes faster (a quicker UTF-8 check); the other columns did not change.

From Python, fstx writes about 1.6 times faster than Feather (lz4) and about 7 times faster than Parquet, and
its file is the smallest. Reading is slower than Parquet (0.24 s against 0.20 s; on one thread 0.60 s against
0.42 s). The R package still writes the same table in about half the time Python takes: what is left is the
Python side's own preparation (about 0.1 s of casts and checks, run one column after another), not the engine.
R's read here makes 10 million R strings (`strings = "eager"`, 2.48 s); its default makes them on first use
(0.09 s for the read itself). `profile="fast"` (no codec) gave the same times at 231 MiB. Feather's two-column
read is noisy (fastest of the 5 runs 0.06 s, median 0.27 s). The pandas read's median rose to 0.57 s (fastest
0.51 s; 0.49 s before); not looked into.
