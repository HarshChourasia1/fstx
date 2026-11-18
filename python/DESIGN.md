# fstx for Python: design

Status: proposal, 17 Nov 2026, for the Python port of fstx 1.0.0 (file format 1.0, format version 3). Covers the
Python API, how every R type maps to and from Arrow, what a round trip R -> Python -> R keeps, and the golden
files that test it (`python/tests/golden/`). The build itself (packaging, compiler setup) is a separate piece of
work on branch `python`.

Facts below marked "checked" were run on 17 Nov 2026 with R 4.5.1 + fstx 1.0.0 and Python 3.14.4 + pyarrow
25.0.1 + pandas 3.0.2. Everything else is the proposal.

## 1. Principles

1. **pyarrow.Table is the core type.** Every read returns one; every write takes one (or anything that converts
   to one: a pandas or polars DataFrame, any object with `__arrow_c_stream__`). pandas and polars are helpers on
   top, not separate code paths.
2. **Reuse the C++ engine as it is.** The binding calls the plain C API in `src/fstx_core.h` (open, plan, read,
   colsrc, filter, writer, compact) and the Arrow stream in `src/r_arrow.cpp`, which already builds without R
   (`-DFSTX_ARROW_NO_R`) and uses only the Arrow C ABI of `src/arrow_abi.h`. pyarrow imports that stream with
   `pa.RecordBatchReader._import_from_c` (or the PyCapsule `__arrow_c_stream__` protocol). No R code, no second
   decoder. The file format does not change: a file written by Python is an ordinary fstx 1.0 file.
3. **Keep R's meaning in Arrow metadata.** Arrow cannot say "Date stored as double", "POSIXct with tzone \"\"",
   "integer64", "factor with an NA level" or "data.table key". The reader puts what the file stores into field and
   schema metadata (section 4); the writer reads it back. With the metadata intact, R -> Python -> R is identical()
   for every case in section 3 that is not marked lossy.
4. **Never run R code.** Attribute blobs in R serialization (spec 9.4) are never decoded in Python; they travel as
   opaque bytes. FXA1 blobs are decoded and encoded in Python (a few dozen lines; spec 9.4 is complete).
5. **Refuse rather than guess.** A type with no fstx 1.0 storage (binary, list, struct, decimal, ...) is an error
   that names the column, as `write_fstx()` in R does.

## 2. API

Module `fstx`. Rows are **0-based** in Python (R's are 1-based); everything else follows the R names.

```python
import fstx

t = fstx.read_fstx(path, columns=None, rows=None, filters=None, *,
                   threads=None, verify=True, as_factor=False, attributes=True,
                   datetimes="arrow", invalid_utf8="replace")          # -> pyarrow.Table

fstx.write_fstx(data, path, *, profile="balanced", compress=None, level=None,
                threads=None, append=False, int64="auto")               # -> None

m = fstx.metadata(path)        # FileMeta: nrow, ncol, row_groups, pages, profile, format_version,
                               # file_size, schema (pyarrow.Schema with the metadata of section 4),
                               # columns (list of ColumnMeta: name, phys, logical, tzone, levels,
                               # compressed_bytes, raw_bytes, pages, r_class)
p = fstx.pages(path)           # pyarrow.Table, one row per page, as R's fstx_pages()

with fstx.open(path) as f:     # the lazy form (R: read_fstx(lazy = TRUE))
    f.schema, f.num_rows
    f.read(columns=..., rows=..., filters=...)                          # -> pyarrow.Table
    f.to_batches(columns=None, batch_rows=None, filters=None)           # -> pyarrow.RecordBatchReader
    # f.__arrow_c_stream__(): polars.scan / duckdb read it lazily, a row group at a time

fstx.which(path, filters, start=0, stop=None)     # numpy uint64 array of 0-based matching rows
fstx.explain(path, filters, start=0, stop=None, exact=False)   # dict, as R's fstx_explain()
fstx.compact(path, merge=False, threads=None)

fstx.to_pandas(table)          # R-aware: nullable dtypes, row names -> index, metadata kept in df.attrs
fstx.to_polars(table)          # polars.from_arrow plus the same casts
```

Arguments, against R's `read_fstx()` / `write_fstx()`:

| R | Python | note |
|---|---|---|
| `columns =` names or 1-based positions | `columns=` names or 0-based positions | duplicate names: first match, as R |
| `from =, to =` (1-based, inclusive) | `rows=slice(start, stop)` or `range` | half-open, 0-based |
| `rows =` (1-based, any order, repeats) | `rows=` list / numpy array of 0-based ints | same engine call (`fstx_plan_create_rows`) |
| `where = x > 5 & y == "a"` | `filters=[("x", ">", 5), ("y", "==", "a")]` | AND only, as R. Ops `== != < <= > >= in is_null not_null`; `between` is two terms. Values converted as R's `.term_pred()` does: strings for Date / timestamp columns parse as ISO dates, factor values are level names, `<` on a factor needs `ordered`, `None` in a comparison matches nothing, `in` may hold `None` (NA rows) and `nan` (NaN rows). integer64, difftime and other classed columns cannot be filtered (R refuses them too) |
| `lazy = TRUE` | `fstx.open()` | no ALTREP in Python; the open file pages through mmap |
| `strings = "auto" / "deferred" / "eager"` | not ported | R string-cache details; Python always builds Arrow strings |
| `as_factor = TRUE / names` | same | dictionary-coded string columns come back as `dictionary<int32, string>`; levels sorted as R |
| `attributes = FALSE` | same | drops the opaque R-serialization blobs from the metadata (FXA1 stays) |
| `verify`, `threads` | same | `threads=None` = all cores |
| `profile`, `compress`, `level`, `append` | same | `compress` in `"zstd"`, `"lz4"`, `"none"`. **pcodec does not exist in fstx 1.0**: codec 4 is reserved and refused |
| `options(fstx.verify_nulls = TRUE)` | `fstx.set_options(verify_nulls=True)` | |
| - | `datetimes="arrow"` / `"float"` | `"float"` returns Date / POSIXct as their stored float64 / int32 (exact; section 3.4) |
| - | `invalid_utf8="replace"` / `"error"` / `"binary"` | section 3.3 |
| - | `int64="auto"` / `"integer64"` / `"double"` | section 3.6 |

Character columns come back as `string` (or `large_string` when the column's text could pass 2 GB), never as
dictionary unless `as_factor` asks: R's Arrow stream switches to dictionary when every page is dictionary-coded,
which makes the schema depend on the data. Python decodes those to plain strings so one file always gives one
schema.

## 3. Type mapping

Each row: how R holds it, how the file stores it (spec 4.2: phys / logical / levels / tzone, plus the attribute
blob "attrs" of spec 9.4), what Python returns, what Python writes back, and whether R -> Python -> R gives an
identical() table through pyarrow ("RT"). "meta" = the field metadata of section 4. The golden case that tests the
row is in brackets.

### 3.1 Logical, integer, double

| R | file | Python read | Python write back | RT / lost |
|---|---|---|---|---|
| logical, NA | phys 2 (int32 0/1, NA = 0x80000000) | `bool`, NA -> null | `bool` -> logical | yes [basic] |
| integer, NA | phys 0 int32, NA = 0x80000000 | `int32`, NA -> null | `int32` -> integer. A non-null INT32_MIN cannot be stored (it is R's NA): error | yes [basic] |
| double | phys 1 float64 | `double` | `double` -> double, bits kept | yes [basic] |
| double NA_real_ (0x7FF00000000007A2) | missing-value block marks it | null | null -> NA_real_ | yes |
| double NA_real_ + 1 (quiet NA, 0x7FF80000000007A2) | stored as a value, counted in null_count (spec 4.5) | null (any NaN whose low word is 1954, as R and fstx's Arrow stream) | NA_real_ 0x7FF00000000007A2 | identical() TRUE, bits differ (checked: identical() ignores the payload) [basic] |
| NaN (0x7FF8000000000000) | a value, not missing | `nan` value, not null | `nan` -> NaN | yes through pyarrow. **Through pandas, no**: pandas has one NaN for both, and `from_pandas` turns NaN into null, so R's NaN comes back NA [basic] |
| -0, Inf, -Inf, subnormal | values | same bits | same bits | yes [basic] |

### 3.2 Characters

| R | file | Python read | Python write back | RT / lost |
|---|---|---|---|---|
| UTF-8 / ASCII strings, `""`, NA | phys 3, UTF-8; NA in the missing-value block, `""` is length 0 not missing | `string`, NA -> null, `""` -> `""` | `string` / `large_string` / `string_view` -> character | yes [basic, strings_*] |
| non-ASCII in Indian scripts (Devanagari, Bengali, Tamil, Telugu, Kannada, Malayalam, Gujarati, Gurmukhi, Odia, Urdu, ZWJ conjuncts, 4-byte characters) | UTF-8 bytes | same text | same bytes | yes [strings_indic] |
| latin1-marked | converted to UTF-8 by the R writer | UTF-8 text | UTF-8 | identical() TRUE; R marks them UTF-8 after any read, Python or not [strings_latin1] |
| "native" | UTF-8 locale: stored as is; other locales: converted to UTF-8 | text | UTF-8 | as latin1 |
| "bytes"-marked | **refused by the R writer** (checked: "strings marked as 'bytes' encoding, which cannot be stored") | - | - | no file holds them |
| not valid UTF-8 (native strings with bad bytes in a UTF-8 session) | **stored as they are** (spec 9 rule 7; checked) | `invalid_utf8="replace"` (default): bad bytes -> U+FFFD, one warning per column; `"error"`: refuse; `"binary"`: the column comes back as `binary`, bytes kept. pyarrow must never hold invalid UTF-8 in a `string` array (checked: casting it fails; later ops would too) | `binary` columns with meta from fstx -> character with the same bytes; other `binary` -> refused | **no** by default; yes with `"binary"` [strings_invalid_utf8] |
| long strings (over 64 KB) | as any string; the writer refuses strings over 1 GB | `string` | same; over 1 GB refused | yes [strings_long] |
| NUL byte | impossible in R; corrupt in a file | - | refused | - |

### 3.3 Factors

| R | file | Python read | Python write back | RT / lost |
|---|---|---|---|---|
| factor | logical 1, phys 0: 1-based codes, NA code = missing, `levels` in the footer | `dictionary<indices=int32, values=string, ordered=0>`; codes - 1; NA -> null | dictionary -> factor; levels = dictionary values in order | yes [factors] |
| ordered factor | same + `ordered` | same, `ordered=1` | ordered dictionary -> ordered factor | yes [factors] |
| unused levels | levels kept | kept in the dictionary | kept | yes [factors] |
| no levels at all | levels empty | empty dictionary, all null | same | yes [all_na] |
| NA as a level (`exclude = NULL`) | **not native**: the R writer only stores levels without NA natively, so this is phys 0 with `levels` and `class` in the FXA1 blob (checked) | dictionary whose values hold a null; a code pointing at it is a value, not a null (checked: pyarrow allows this) | dictionary with a null value -> int32 + FXA1 blob {levels with NA, class}, as R does | yes through pyarrow. **pandas cannot hold it** (checked: "Categorical categories cannot be null"); `fstx.to_pandas` turns the column into strings with a warning [factor_na_level] |
| class `c("myfac", "factor")` | not native: int32 + blob | dictionary (any class vector containing "factor" with string levels); class in meta | class from meta | yes with meta [classes_blob] |
| pandas Categorical / polars Categorical, Enum | - | - | factor (ordered when ordered); dictionary of non-strings -> written as its decoded values | - |

### 3.4 Dates and times

| R | file | Python read (`datetimes="arrow"`) | Python write back | RT / lost |
|---|---|---|---|---|
| Date as double (`as.Date()`) | logical 2, phys 1 (days) | `date32[day]`; meta phys = double | date32 -> Date **as double** (what `as.Date()` gives) unless meta says integer | yes [dates] |
| Date as integer | logical 2, phys 0 | `date32[day]`; meta phys = integer | integer if meta says so | yes only through meta: date32 alone cannot tell, and identical() sees integer vs double (checked) [dates] |
| Date fractional (1.5), Inf, -Inf, NaN | double values | date32 floors fractions (as R prints them; as fstx's Arrow stream); Inf / -Inf / NaN -> null | null -> NA | **no** (Inf, NaN -> NA, 1.5 -> 1). Exact with `datetimes="float"` [dates_odd] |
| years 1 to 9999 | double days | date32 holds them | - | yes; pandas keeps them as `datetime.date` objects (checked), `datetime64[ns]` cannot |
| POSIXct with tzone | logical 3, phys 1 seconds, `tzone` | `timestamp[us, tz=<tzone>]`, value = round(seconds x 1e6) half-even (as the stream) | timestamp -> POSIXct double seconds = us / 1e6, tzone = tz | yes when the seconds are whole microseconds [datetimes: UTC, Asia/Kolkata, America/New_York] |
| POSIXct, tzone `""` | `tzone` present and empty (checked) | `timestamp[us]` (naive); meta `tzone: ""` | tzone `""` from meta | yes with meta [datetimes] |
| POSIXct, no tzone attribute | `tzone` absent (checked) | `timestamp[us]` (naive); meta says absent | no tzone from meta | yes with meta [datetimes] |
| sub-microsecond seconds (`Sys.time()`) | float64 | rounded to the microsecond | us / 1e6 | **no**: last bits change (checked with the simulated round trip in check_roundtrip.R). Exact with `datetimes="float"` [datetimes_subus] |
| POSIXct Inf, -Inf, NaN | values | null | NA | **no** [datetimes_odd] |
| POSIXct stored as integer, or with a 2-value tzone, or extra classes | not native: int32 / double + blob | `timestamp[us, tz=tzone[1]]` (class contains POSIXct); meta keeps phys and class | from meta | yes with meta [datetimes_odd, classes_blob] |
| POSIXlt | a list: R's `data.frame()` turns it into POSIXct first; a list column is refused | - | - | - |
| difftime (secs, mins, hours, days, weeks), hms | double + blob {class, units} | `double`; meta has class and units. `fstx.to_pandas` keeps float (an option gives `timedelta64`) | double + blob from meta | yes [difftime]; via timedelta64: units become secs |

Naive timestamps written from Python with no fstx meta (data that never came from R) get tzone `"UTC"`, so R
shows the same clock time Python showed. R's own meaning of "no tzone" is "an instant shown in the session's local
time", which Python has no equivalent for.

Fixed UTC offsets (added 18 Nov 2026, Harsh's "do 1-5"). Arrow writes a fixed offset as the time zone `"+05:30"`;
R reads that string as a POSIX TZ rule, where the sign counts the other way (UTC-05:30), and shows the time 11 hours
off. The writer stores such a zone as R's tzone in POSIX form with a name in angle brackets, `"<+0530>-05:30"`
(`"-03:00"` -> `"<-0300>+03:00"`, `"+00:00"` -> `"<+0000>-00:00"`), and the reader turns exactly that form back
into `"+05:30"`, so Python round-trips. Checked on R 4.5.1 and R 4.6.1 (Windows): `format()`,
`as.POSIXlt()$hour` / `$gmtoff` and an R write -> read are right for +05:30, -03:00, +00:00, +05:45, -09:30,
+14:00, -12:00 and +01:00 between the years 1900 and 9999; R warns "unknown timezone '<+0530>-05:30'" whenever it
formats such a column (its zone database has no file of that name, so it falls back to the POSIX rule, which is
right). Named zones (Asia/Kolkata, UTC, ...) are unchanged. The metadata contract is unchanged: `fstx.r`'s
`tzone` is what the file holds (the POSIX form); only the Arrow type carries `"+05:30"`. A file that holds
`"+05:30"` itself (written by R, or by this package before 18 Nov 2026) reads as `"+05:30"` and is written back as
it was. pandas turns a zero offset into UTC; with the metadata the writer keeps the file's `"<+0000>-00:00"`.
polars has no fixed offsets: `fstx.to_polars` returns them as UTC (same instants) with a warning (section 8).

### 3.5 integer64 (bit64)

| R | file | Python read | Python write back | RT / lost |
|---|---|---|---|---|
| integer64, NA = INT64_MIN | **phys 1 double page** holding the int64 bit patterns, `class = "integer64"` in the FXA1 blob (checked) | `int64`, made by reinterpreting the decoded float64 bits; null **only** where the bits are INT64_MIN. The page's missing-value block and null_count must be ignored for this column | int64 -> double page of the same bits + blob `class = "integer64"` | yes [integer64] |
| values whose bits are R's NA_real_ (9218868437227407266), quiet NA (9221120237041092514), Inf or NaN | the R writer marks the NA_real_ one missing and counts both NA patterns in null_count (checked: page nulls = 2 for 1 real NA) | must come back as these numbers, not null | same bits | yes, only if the reader ignores validity as above [integer64] |
| filters, page min / max | statistics are of the doubles, meaningless here | filters on integer64 refused (as R) | the writer's statistics for these pages are wrong by design; harmless while filters refuse the column | - |

### 3.6 Arrow / pandas types that do not come from R

| Python | written as | read back in Python as | note |
|---|---|---|---|
| `bool` | logical | `bool` | |
| `int8`, `int16`, `uint8`, `uint16`, `int32` | integer | `int32` | type widens; INT32_MIN refused |
| `uint32`, `int64`, `uint64` | `int64="auto"`: integer when every value fits int32 (and none is INT32_MIN), else integer64; `"integer64"`: always integer64; `"double"`: double (refused if a value is past 2^53) | `int32` or `int64` | pandas makes every integer column int64, so `"auto"` keeps ordinary data readable in R without bit64. uint64 above 2^63 - 1 refused |
| `float16`, `float32`, `float64` | double | `double` | float32 values widen exactly |
| `decimal128/256` | refused | | cast to double or string first |
| `string`, `large_string`, `string_view` | character | `string` | pandas 3 `str` arrives as `large_string` (checked) |
| `binary`, `large_binary`, `fixed_size_binary` | refused (fstx 1.0 has no binary type; phys 5 is reserved) | | except `binary` that fstx itself returned with `invalid_utf8="binary"` |
| `dictionary<*, string>` | factor | `dictionary<int32, string>` | |
| `date32` | Date (double) | `date32` | |
| `date64` | Date (double, fractional when not midnight) | `date32` | |
| `timestamp[s/ms/us/ns, tz]` | POSIXct (double seconds) | `timestamp[us, tz]` | ns: precision past double's (about 0.2 us today) is lost; unit becomes us; a fixed offset `"+05:30"` is stored as tzone `"<+0530>-05:30"` (section 3.4) |
| `time32`, `time64` | hms (double seconds, class `c("hms", "difftime")`, units `secs`) | `double` + meta | R prints it as hms with the hms package, as difftime without |
| `duration[unit]` | difftime, units `secs` | `double` + meta | |
| `null` (all-null column) | logical, all NA | `bool` | as R's default NA |
| `list`, `large_list`, `struct`, `map`, `union`, extension types | refused | | R refuses list columns too (checked) |
| pandas `object` | whatever pyarrow infers; mixed types refused by pyarrow | | |
| pandas `Period`, `Interval`, sparse | refused | | |

### 3.7 Tables, row names, names and attributes

| R | file | Python | Python write back | RT / lost |
|---|---|---|---|---|
| data.frame / data.table / tibble class | table blob (FXA1) `class` | schema meta `fstx.table_attrs` (the blob, base64); class shown in `FileMeta` | blob from meta; no meta -> `class = "data.frame"` | yes [basic, tibble] |
| automatic row names | not stored | none | none | yes |
| character row names | table blob `row.names` | kept in the blob; a read of some rows (`rows=`, `filters=`) rewrites the blob so the names follow the rows read; `fstx.to_pandas` makes them the index | from the blob when the row count still matches, else dropped; from pandas: a string index becomes row names, a RangeIndex 0..n-1 none | yes [rownames_char]. Risk: moving rows of a pyarrow.Table after the read (`take`, `sort_by`, `filter`) keeps its metadata, so row names would pair with the wrong rows; select rows in `read_fstx`, or use pandas when rows move |
| integer row names that are not automatic (`x[c(4, 2), ]`) | table blob, integer | as above; integer index | as above | yes [rownames_int] |
| data.table key (`sorted`) | table blob | kept | kept only if the key columns are still sorted (checked by the writer) and rows were not dropped | yes when still sorted [datatable] |
| data.table index (`index`) | table blob (order vectors) | kept | **always dropped**: the writer cannot tell whether rows moved; data.table rebuilds indices when needed | **no** [datatable] |
| other table attributes (plain vectors) | FXA1 | decoded into `FileMeta`, blob kept | blob kept | yes [attrs_fxa1] |
| column attributes (labels, units, comment, named vectors) | column FXA1 blob | field meta `fstx.attrs` (base64) + decoded in `ColumnMeta` | blob kept when the column type is unchanged | yes [attrs_fxa1] |
| list-valued attributes (any attribute FXA1 cannot hold) | R serialization blob | opaque bytes (base64) in meta, never decoded; `attributes=False` drops them | written back unchanged (table blob: only if the row count is unchanged, since it may hold row names) | yes when unchanged [attrs_serialized] |
| duplicate names, `""` | as written | kept (pyarrow allows both) | kept | yes [names] |
| name `NA` | R writes it as `"NA"` | `"NA"` | `"NA"` | already not identical inside R (checked: NA name reads back as "NA") |
| 0 x 0, 0 rows, 0 columns with rows | as written | 0 columns with rows: `RecordBatch.from_struct_array` of an empty struct gives a table of n rows (checked) | row count kept | yes [empty_0x0, zero_rows, zero_cols_5rows] |
| raw, complex, list, matrix columns | **refused by R** (checked) | - | - | no file holds them |

## 4. Arrow metadata

One contract for the reader and the writer (keys and helpers: `fstx/_meta.py`). Every value is ASCII text, so
other libraries can carry a table read by fstx: polars reads Arrow metadata as UTF-8 and refuses anything else
(`pl.from_arrow(read_fstx(...))` works on every file, checked on R-serialized and non-ASCII blobs).

Field metadata:

- `fstx.r`: JSON `{"phys", "logical", "class"?, "tzone"?}`: how R holds the column. `phys` is integer / double /
  logical / character; `logical` is none / factor / Date / POSIXct; `class` is R's class vector (absent when
  there is none, or when an R-serialization blob hides it); `tzone` is there only for a POSIXct column stored
  natively with a tzone attribute (`""` = stored empty, key absent = no attribute). Examples:
  `{"phys":"integer","logical":"none"}`, `{"phys":"double","logical":"Date","class":["Date"]}`,
  `{"phys":"double","logical":"POSIXct","class":["POSIXct","POSIXt"],"tzone":""}`,
  `{"phys":"double","logical":"none","class":["integer64"]}`.
  The reader puts it on every column. The writer accepts a column without it and writes it as the plain data it
  is (section 3.6).
- `fstx.attrs`: the column's attribute blob (FXA1 or R serialization, spec 9.4), base64.

Schema metadata:

- `fstx.table_attrs`: the table's attribute blob, base64.
- `fstx.nrow`: the file's row count, decimal text. Only on a read of the whole file whose table blob is R
  serialization: nobody here can look into such a blob, so the writer keeps it only for a table that still has
  this many rows (or when the bytes `row.names` do not occur in it) and otherwise drops it with a warning.

The writer also takes both blobs as raw bytes: a raw blob is never valid base64 (FXA1 has the byte 0x01 at offset
3, R serialization a line feed at offset 1), so the two cannot be confused.

Row names live in the table blob and nowhere else. A read of every row in file order hands the blob out as
stored. Any other read (`rows=`, `filters=`, a slice) rewrites an FXA1 blob: `row.names` become those of the rows
read, in the order read; repeats are made unique as R's `x[rows, ]` does (`r2`, `r2.1`, ...); automatic row names
get the new count. So a partial read followed by a write gives R the row names of exactly those rows (checked
in R). An R-serialization blob cannot be rewritten: it is handed out without `fstx.nrow`.

The writer uses `fstx.r` only when it agrees with the Arrow type (a `date32` field with `phys: integer` writes an
int Date; a `string` field that still carries Date meta ignores it and warns). Blobs: column blobs are kept when the
Arrow type is the one the reader made; table blobs are re-encoded (FXA1) with row names, key and index dropped as
section 3.7 says, and kept byte for byte when nothing was dropped.

pyarrow keeps both kinds of metadata through `select`, `slice`, `filter`, `take`, `sort_by` and `concat_tables`
(checked); a column replaced with `set_column` loses its field metadata while the schema metadata stays (checked).

pandas: `from_pandas` builds its own metadata, so `fstx.to_pandas` stores ours under one key, and the writer
looks there:

    df.attrs["fstx"] = {"table_attrs": base64 text or None, "nrow": int or None,
                        "columns": [{"name": str, "r": dict or None, "attrs": base64 text or None}, ...]}

It is plain JSON data. Stored row names become the index, and on a write the index decides the row names
(distinct strings, or distinct whole numbers other than 0..n-1; a RangeIndex 0..n-1 means none). Column entries
are matched by position and name, else by a name that occurs once, so dropping or reordering columns keeps
them. `write_fstx(fstx.to_pandas(read_fstx(p)), q)` loses nothing of how R held the table; what pandas itself
cannot hold is in section 8.

polars has no metadata and no index: `fstx.to_polars` (or `pl.from_arrow`) gives the values, and a write from
the polars DataFrame stores plain data (section 8). Write from the pyarrow.Table to keep R's classes,
attributes and row names.

## 5. NA semantics, per type

| type | R missing value | file | Arrow |
|---|---|---|---|
| logical, integer, factor codes | NA_integer_ = 0x80000000 | missing-value block | null; a non-null 0x80000000 in Python is refused on write |
| double | NA_real_ = 0x7FF00000000007A2 (also its quiet form 0x7FF80000000007A2) | block marks the exact pattern; quiet form stored as a value | null for any NaN whose low word is 1954; other NaN are values |
| double NaN | not NA (`is.na` TRUE, `is.nan` TRUE) | value | `nan` |
| Date / POSIXct | NA_real_; Inf / NaN are values | as double | null (and Inf / NaN become null too, section 3.4) |
| character | NA_character_ | block; `""` is a value | null; `""` stays `""` |
| integer64 | INT64_MIN bits (= the double -0.0) | a value (the file sees -0.0) | null only for INT64_MIN |

## 6. Implementation notes for the binding

- Read: `fstx_arrow_stream_open()` gives an `ArrowArrayStream`; Python post-processes per column only where the
  stream cannot know R's meaning: integer64 (reinterpret the float64 values buffer as int64, rebuild validity from
  INT64_MIN), factor in a blob (int32 codes + FXA1 levels -> dictionary), dictionary strings -> plain strings,
  invalid UTF-8 check, metadata. These are zero-copy or one pass over a buffer.
- Two small additions to the shared stream code, both off by default so R is unchanged: (a) a "raw" switch that
  returns Date / POSIXct columns as their stored float64 / int32 (for `datetimes="float"` and for the lossless
  checks), and (b) a switch to skip the NaN-1954 null scan for a column (integer64). Both live in
  `fstx_arrow_args`. Everything else in `src/` is used as it is.
- Write: `fstx_writer_open()` / `fstx_writer_append()` with `fstx_write_column` built from the Arrow schema and
  `fstx_column_data` pointing at the Arrow buffers. Strings go through the `get_strings` callback straight from
  the Arrow offsets and data buffers (it runs on worker threads, so it must not touch Python objects). Arrays with
  nulls are copied once into R's layout (NA sentinels), since the engine takes plain value arrays.
- FXA1 encode / decode in Python per spec 9.4 (count / length checks, three levels, UTF-8 checks). The R
  serialization path is never decoded.
- Appends follow R's rules (`.append_prepare()` in `R/m5.R`): same names, types and classes; factor levels may
  grow at the end; same time zone; no appends to files with stored row names.

## 7. Tests: golden files

`python/tests/golden/` (made by `make_golden.R`; `Rscript make_golden.R` regenerates them byte for byte, checked):

- 35 small `.fstx` files, one per case in section 3 (464 KB with their sidecars), each with a `<case>.json`
  sidecar: per column the stored phys / logical / levels / tzone / blob kind (and blob bytes), the expected Arrow
  type, null count, every value (doubles also as exact bit patterns), a canonical md5 over all values, and the page
  list (kind, encoding, missing-value mode, codec). Columns over 100 rows list their first 12 values; the md5 covers
  the rest. A Python test needs no R. The encoding of the sidecar is described at the top of `make_golden.R`.
- `manifest.json`: per case what it covers and the expected R -> Python -> R result through pyarrow, through
  pandas and through polars ("identical ..." or the reason it is not).
- `format-1.0.json`: the same expectations for the package's own format golden files
  (`tests/testthat/fixtures/golden-1.0`: all 294 mixes of page kind x encoding x missing-value mode x codec,
  appends, compaction, and every reserved number that must be refused, with the message). Python reads those files
  from there.
- Codecs: none, LZ4, Zstandard 1 and 19 (on every page), profiles fast / balanced / small. pcodec cannot be tested
  as written data: fstx 1.0 never writes codec 4; its refusal is in `format-1.0.json`.
- `check_roundtrip.R`: after the Python tests write `from_python/<case>.fstx` (read_fstx -> write_fstx),
  `from_python/<case>.pandas.fstx` (through `fstx.to_pandas`) and `from_python/<case>.polars.fstx` (through
  `fstx.to_polars`), it compares each with the original in R with identical() and reports PASS / KNOWN (expected
  difference) / BETTER / FAIL / ERROR, plus bit-level differences identical() does not see. Exit status 1 on
  FAIL or ERROR.
- `check_native.R`: R reads the files made from data that never came from R, and the partial reads of golden
  files written back (`from_python/native/`), and compares them with the same tables built in R.

## 8. Round trips that will not be identical

Through pyarrow, with metadata kept:

1. Sub-microsecond POSIXct seconds (any `Sys.time()` value): rounded to the microsecond. [datetimes_subus]
2. Date or POSIXct holding Inf, -Inf or NaN: become NA; fractional Dates are floored. [dates_odd, datetimes_odd]
3. Strings that are not valid UTF-8: replaced unless `invalid_utf8="binary"`. [strings_invalid_utf8]
4. data.table secondary index: dropped by the writer. [datatable]
5. Quiet NA_real_ (0x7FF80000000007A2): comes back as NA_real_; identical() TRUE, bits differ. [basic]

With metadata lost (a table rebuilt by compute functions, or pandas without `fstx.to_pandas`): Date as integer
becomes double; tzone `""` and no tzone both become `"UTC"`; integer64 survives (it is a real int64) but difftime,
labels, classes, row names and the table class are gone; Python-made factors lose nothing.

Through pandas, additionally: R's NaN becomes NA in double columns; integer columns with NA turn double unless
nullable dtypes are used (`fstx.to_pandas` uses them; plain `Table.to_pandas()` turns int64 with nulls into float64
and loses precision past 2^53, checked); a factor with an NA level cannot be a Categorical (`fstx.to_pandas`
returns it as strings, with a warning). [basic, difftime, factor_na_level]

Through polars (no metadata, no index): everything listed under "metadata lost" above, and also: a Categorical
keeps only the levels in use, in order of first appearance, and no order; an empty or all-null Categorical has
no levels; a factor with an NA level comes back as strings, as through pandas (found by `test_rmatch.py`,
18 Nov 2026); an Int64 that is empty, all null, or whose values all fit int32 comes back integer (`int64="auto"`;
the last found by `test_rmatch.py`, 18 Nov 2026); repeated column names are refused. Plain logical, integer,
double and character columns, integer64 columns with a value past int32, and Dates stored as double, come back
identical. Per case: `roundtrip_polars` in `manifest.json`.

Fixed UTC offsets (18 Nov 2026): through pyarrow and `fstx.to_pandas` they round-trip (the file holds R's POSIX
form, section 3.4; in R they show the right clock time with an "unknown timezone" warning). polars knows only
named zones (`unable to parse time zone: '+05:30'`): `fstx.to_polars` returns such a column in UTC, the same
instants, with an `FstxWarning`, and written from polars it is a UTC column. [test_rmatch.py, `native_fixed_offset.*`]

## 9. Decisions (Harsh, 17 Nov 2026: "Yes, all four")

1. **int64 from pandas**: `int64="auto"`: int32 when the values fit, else bit64 integer64.
2. **Bad UTF-8 by default**: replace with U+FFFD and warn; the binary mode keeps the bytes.
3. **Row numbers**: 0-based in Python.
4. **Dates and times by default**: Arrow types (date32, timestamp[us]); `datetimes="float"` returns the exact stored values.

Later, with the merge of the read, write and wheels branches (Harsh, 17 Nov 2026: "Yes, merge all", as passed on
by the coordinating session):

5. **Attribute blobs in Arrow metadata**: base64 text (`fstx.attrs`, `fstx.table_attrs`), not raw bytes, so that
   polars and other libraries that read metadata as UTF-8 accept the tables. The writer takes raw bytes as well.
