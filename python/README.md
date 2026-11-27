# fstx for Python

Reads and writes fstx files (the FST-X column-store format of the R package fstx) from Python, on the same C++
engine as the R package. A file written by either language is an ordinary fstx file that the other reads.
Tables are `pyarrow.Table`s; pandas and polars are helpers on top. Python 3.10+, pyarrow 15+; pandas, polars and
numpy are optional. The design, type by type, is in `DESIGN.md`; build and test notes are in `DEV.md`.

## Install

```
pip install fstx
```

Wheels for Windows x64, Linux x86_64 and aarch64, and macOS (Intel and Apple silicon), Python 3.10 to 3.14.

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
`compress` (`"zstd"`, `"lz4"`, `"none"`), `level`, `threads`, `append`, `int64` (see below).

`int64` decides how int64, uint32 and uint64 columns are stored:

- `"auto"` (default): int32 (an R integer) when every value fits, else an int64 column.
- `"native"`: always an int64 column.
- `"integer64"`: R's bit64 integer64 in the fstx 1.0 layout (the bits in a double column), for files fstx 1.0
  must read.
- `"double"`: float64; refused when a value is past 2^53.

An int64 column needs fstx 1.2 or later to read (R or Python); fstx 1.0 refuses the file with "Update fstx".
A column read from an fstx file keeps its layout when written back, and an append follows the file. A uint64 value
above 2^63 - 1, and a non-null -2^63 (the missing value), are refused.

How R's types arrive:

| R | Python (pyarrow) | pandas (`to_pandas`) |
|---|---|---|
| logical, integer, double, character | `bool`, `int32`, `double`, `string` | `boolean`, `Int32`, `Float64`, `str` |
| factor (ordered) | `dictionary<int32, string>` (ordered) | `category` |
| Date | `date32` | `datetime.date` objects |
| POSIXct with tzone | `timestamp[us, tz]` | `datetime64[us, tz]` |
| bit64 integer64 (int64 column, or the fstx 1.0 layout) | `int64` | `Int64` |
| list of raw vectors (also blob) | `binary` (`large_binary` for columns past 2 GB) | `bytes` objects |
| difftime, labels, other attributes | the values, with the attributes in the field metadata | the same, in `df.attrs["fstx"]` |

Written from Python: `binary`, `large_binary`, `binary_view` and `fixed_size_binary` are stored as binary (any
bytes, NUL included; `fixed_size_binary` reads back as `binary`). Binary columns need fstx 1.2 or later to read.
Plain `Table.to_pandas()` turns an int64 column with nulls into float64 (pyarrow's rule; inexact past 2^53);
`fstx.to_pandas()` gives `Int64`.

R's NA is a null everywhere; NaN stays NaN through pyarrow and polars (pandas 3 makes it missing). A table read
here and written back keeps how R held it (classes, time zones, integer vs double Dates, row names): write from
the pyarrow.Table or from `fstx.to_pandas()`. The few things that do not survive are listed in `DESIGN.md`
section 8.

Rows are 0-based in Python (R's are 1-based). Filters are AND-ed terms `(column, op, value)` with ops
`== != < <= > >= in is_null not_null`, as R's `where =`. On int64 columns they compare exactly (Python ints and
floats alike); binary columns take `== != in is_null not_null` with `bytes` values.

## Speed

Measured 27 Nov 2026 on one desktop: Intel Core Ultra 5 245K (14 cores), 31.4 GiB RAM, NVMe SSD, Windows x64;
pyarrow 26.0.1, vortex-data 0.95.0, pylance 14.0.3, duckdb 2.0.1, polars 2.2.0. fstx is the development build of
that day (the 1.2 speed work, before the int64 and binary types and the string chooser fix), profile "balanced".
Arrow tables out, all cores. Each case runs in a fresh process; a time is the median of 3 runs, the better of 2
rounds; page cache warm.

The mixed table, 5,000,000 rows and 8 columns (numbers, a date, strings, a factor, a logical), seconds:

| | fstx | Parquet (snappy) | Parquet (zstd 3) | Vortex | Lance | DuckDB on Parquet | Polars on Parquet |
|---|---:|---:|---:|---:|---:|---:|---:|
| write | **0.0954** | 0.869 | 1.29 | 0.346 | 0.969 | 0.442 | 0.151 |
| full read | **0.0322** | 0.133 | 0.139 | 0.0422 | 0.0931 | 0.190 | 0.156 |
| one column | 0.0061 | 0.0349 | 0.0489 | **0.0053** | 0.0155 | 0.0270 | 0.0191 |
| 1% row window | 0.0039 | 0.0430 | 0.0533 | **0.0032** | 0.0057 | 0.0251 | 0.0151 |
| filter (about 2% of rows) | **0.0173** | 0.121 | 0.139 | 0.0306 | 0.109 | 0.0785 | 0.0494 |
| file size, MB | **64.8** | 128 | 88.1 | 83.7 | 147 | 124 | 88.7 |

On the numeric table (10,000,000 rows x 6) and the text table (2,000,000 rows x 4 strings) fstx was the fastest in
the same five tests except one: the text table's one-column read (Vortex 0.0189 s, fstx 0.0190 s). Its files were
the smallest of the three tables. Reads of a few milliseconds are noisy: for some of them the two rounds differed by
more than 20% (fstx's text window: 0.0015 and 0.0058 s). fstx's full reads and filtered reads used the least extra
memory of all formats; its writes used more than pyarrow's Parquet writer (text 86 against 22 MB, mixed 122
against 69 MB).

## Changes

### 1.2.0

- New column types, shared with the R package fstx 1.2: int64 (R's bit64 integer64) and binary. Files with such a
  column need fstx 1.2 or later; every other file 1.2 writes reads in 1.0.
- `int64="auto"` now stores a column whose values do not fit int32 as an int64 column (1.0: integer64 in the 1.0
  layout, which `int64="integer64"` still writes). New `int64="native"`.
- Exact filters on int64 columns; `==`, `!=`, `in` and the null tests on binary columns.
- `FileMeta.required_features` and `optional_features`; column type names `"int64"` and `"binary"`.
- Faster reads, filters and writes (numbers above). Windows wheels are built with clang-cl.
- Fixed: a file whose string column has dictionary-coded row groups and a plain one (for example a short last
  row group) could not be read; every read path failed with "dictionary and plain pages mixed".
- The writer's choice between plain and FSST string pages compares the two as pages of their real size; it took
  the larger encoding for some text columns.

### 1.0.0

- First release.
