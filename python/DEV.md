# fstx for Python: build and layout (developer notes)

Step 1 of the Python port (17 Nov 2026, branch `python`): the package skeleton. It compiles the fstx C++
engine (`src/`, no R) into a Python extension and can open a file and report its metadata. Reading columns,
writing and wheels build on this layout. The API it grows into is in `DESIGN.md`.

## What was built and tested (17 Nov 2026, desktop, Windows x64, Python 3.12)

| compiler | engine self-test (`coretest`, 200,000 rows) | wheel | pytest |
|---|---|---|---|
| Microsoft cl 19.44 (Build Tools 2022, MSVC 14.44) | 470 checks passed, 0 failed | `fstx-1.0.0.dev0-cp312-abi3-win_amd64.whl` (227 KB) | 132 passed |
| clang-cl 19.1.5 (Build Tools LLVM component) | 470 checks passed, 0 failed | same name | 132 passed |

- The files coretest writes have the same hashes under cl, clang-cl and the earlier GCC (Rtools45) build, for
  scalar, SSE2, SSE4 and AVX2: none=1c93601507cec4f4 lz4=1554f8a1c20f03c6 zstd=6fe67b396e676cfa
  compacted=3aea9655391d7758. So the Windows Python build writes the bytes the R package writes.
- Highway dynamic dispatch works under both: AVX2, SSE4, SSE2 compiled, AVX2 picked on this CPU; the kernel
  self-test passes under each target and with SIMD off.

Not built or tested in step 1: Linux, macOS, ARM, Python other than 3.12, the abi3 wheel on a newer Python, an
sdist, `R CMD build` after the `.Rbuildignore` change. Step 5 (wheels, below) covered the sdist, 3.11, abi3 on
3.14, Linux x86_64 under WSL, and the R side: `R CMD build` (R 4.6.1, Rtools45) of the tree with `python/` in it
gave `fstx_1.0.0.tar.gz` with no `python/` entry, `R CMD INSTALL` had no warning, and a write / read round trip
(none, lz4, zstd) came back identical. Python 3.9 is dropped (floor 3.10).

## Step 2: reading (17 Nov 2026, branch `py-read`)

`fstx.read_fstx`, `File.read`, `File.to_batches`, `File.schema`, `FileMeta.schema`, `ColumnMeta.r_class`,
`fstx.to_pandas`, `fstx.to_polars` and the FXA1 decoder. Tested on the desktop (Windows x64, Python 3.12,
pyarrow 26.0.1, pandas 3.1.1, polars 2.1.0, numpy 2.5.5):

| compiler | wheel | pytest |
|---|---|---|
| Microsoft cl (Build Tools 2022) | `fstx-1.0.0.dev0-cp312-abi3-win_amd64.whl` (340 KB) | 413 passed (132 from step 1, 281 in `test_read.py`) |
| clang-cl (Build Tools LLVM) | same name (421 KB) | 413 passed |

R with the engine change (R 4.6.1, package built and installed into its own library): `test-arrow.R`,
10 tests, 356 expectations, 0 failed, 1 skipped. The rest of the R suite and coretest were not rerun (the
change touches `r_arrow.cpp` and `arrow_abi.h` only). Not tested: Linux, macOS, Python other than 3.12,
pyarrow older than 26 (the floor `>=15` is where `RecordBatchReader.from_stream` appeared; not run), files
with string columns over 2 GB (the `large_string` path), whether the GIL release speeds up threads (only that
8 threads reading at once give right answers).

How a read goes: `_core.stream()` opens the engine's Arrow C stream (`fstx_arrow_stream_open`) with the GIL
released and returns an `arrow_array_stream` PyCapsule; pyarrow imports it (`RecordBatchReader.from_stream`)
and pulls batches with the GIL released; `_read.py` finishes each batch (strings, integer64, classes kept in
the attribute blob). Filters go through `_core.filter()` (`fstx_filter`), which returns the matching row
numbers; those are then read through the stream's row-list path.

Metadata on what is returned: see "One contract for reading and writing" at the end (the merge replaced
what this step first did: raw blobs, `fstx.r` only on some columns, `fstx.row_names`, `df.attrs["fstx_schema"]`).

What does not come back as R holds it:

- Date and POSIXct with `datetimes="arrow"`: fractional days are floored, seconds rounded to the microsecond,
  Inf / NaN become null. `datetimes="float"` gives the stored numbers.
- Strings that are not valid UTF-8: replaced with U+FFFD plus a warning (default), or `binary`, or an error.
- A factor with NA as a level reads as a dictionary with a null value (kept); pandas and polars cannot hold
  it, so the two helpers return strings with a warning.
- pandas 3 turns NaN into `<NA>` in `Float64` unless `future.distinguish_nan_and_na` is on.
- polars: repeated column names are refused.
- `as_factor` sorts levels by code point (R sorts in the session's locale).
- Filters: `is_null` / `not_null` follow R's `is.na()` (NaN counts in double columns). integer64, difftime
  and other classed columns, and columns whose attributes are R serialization, cannot be filtered (as in R).
- Errors raised while a batch decodes carry the engine's message but no code (`FstxError.code` is None): the
  Arrow stream keeps only the message.
- `to_batches`: `invalid_utf8="binary"` makes every character column binary; no `as_factor`; rows only as a
  slice or range; a read of zero rows yields no batch (the schema still has the types).

## Build and test

From the repo root, in a shell with a C++17 compiler (Windows: "x64 Native Tools Command Prompt", or
`vcvars64.bat`), in a virtual environment:

```
python -m pip install scikit-build-core nanobind pytest ninja
python -m pip wheel python -w dist --no-deps -v
python -m pip install --force-reinstall --no-deps dist\fstx-1.0.0.dev0-cp312-abi3-win_amd64.whl
python -m pytest python/tests -q -rs
```

- Run pytest from the repo root, not from `python/`: there the source folder `fstx/` (which has no compiled
  `_core`) hides the installed package.
- clang-cl instead of cl: `set CC=clang-cl` and `set CXX=clang-cl` (with `CMAKE_GENERATOR=Ninja`) before
  `pip wheel`.
- The engine's own test program, no Python:
  `cmake -S python -B build/ct -G Ninja -DFSTX_BUILD_PYTHON=OFF -DFSTX_BUILD_CORETEST=ON -DCMAKE_BUILD_TYPE=Release`,
  `cmake --build build/ct`, `build/ct/coretest <empty folder> 200000`. Compare its hash lines with the ones above.
- Desktop helper scripts (not in the repo): `C:\Users\31har\fstx\v1\py\scripts\` (`env.cmd`, `coretest.cmd`,
  `wheel.cmd <msvc|clangcl>`, `guarded.ps1`); sources in `...\py\fstx`, venv in `...\py\venv`, logs in `...\py\build`.

## Wheels, sdist and version

- Version: one source, `version` in `python/pyproject.toml`. A release: set it there (say `1.0.0`), commit, tag
  `py-v1.0.0`, push the tag. The workflow's sdist job stops when tag and pyproject disagree. The engine version
  (written into every file's created_by) stays the one in `DESCRIPTION`; the Python version can move on its own
  (a binding-only fix is a Python release, not an engine one).
- sdist: `python python/tools/make_core.py` (copies the engine into `python/core/` and refreshes `python/LICENSES/`),
  then `python -m build --sdist python --outdir dist`. It builds anywhere: CMake takes `core/` when it is there,
  `../src` otherwise. Without make_core the sdist has no engine and its build stops at "engine sources not found".
  `python/core/` is a generated, git-ignored copy that the sdist needs; keep it. While it exists, builds in the
  tree use it and not `src/`, so rerun make_core after any `src/` edit (`-DFSTX_CORE_DIR=../src` overrides it,
  as the coretest build must: its path is `<core dir>/../tools/coretest`).
- Wheels: `.github/workflows/python-wheels.yml` (cibuildwheel), on a `py-v*` tag or by hand, never on plain pushes.
  Windows x64 (cl), Linux x86_64 and aarch64 (manylinux_2_28; aarch64 on the native `ubuntu-24.04-arm` runner),
  macOS x86_64 (`macos-15-intel`, 10.15+) and arm64 (`macos-15`, 11+). Per platform: a cp310 and a cp311 wheel
  and one cp312-abi3 wheel, which cibuildwheel also tests on 3.13 and 3.14. pytest runs on every wheel. The sdist
  job also unpacks the sdist away from the tree, builds a wheel from it and runs pytest. Artifacts only: the
  PyPI job is in the file as comments, not enabled.
- Licences (PEP 639, 18 Nov 2026): `license = "MIT"` and `license-files` in `[project]` (scikit-build-core 0.11+
  in `build-system.requires`); `python/LICENSE` (fstx, MIT) and `python/LICENSES/` go into the sdist and into the
  wheel's `.dist-info/licenses/`, with `License-Expression:` and one `License-File:` line each in the metadata.
  The licence classifier is gone (PEP 639 does not allow it next to an expression). `readme = "README.md"` is the
  long description.

Checked on 17 Nov 2026 (desktop): sdist (244 entries, engine in `core/`) unpacked outside the git tree built
`cp312-abi3-win_amd64` and `cp311-cp311-win_amd64` wheels with cl; pytest 132 passed on 3.12, 3.11 and with the
abi3 wheel on 3.14. The same sdist in WSL Ubuntu 24.04 (GCC 13.3) built `cp312-abi3-linux_x86_64`, 132 passed, AVX2
dispatch (a plain Linux wheel; auditwheel puts it at manylinux_2_38 because of the newer glibc there, the
manylinux_2_28 image builds the real one). Not run: the workflow itself, so macOS, Linux aarch64, manylinux,
cp310, cp313, abi3audit and delvewheel are first tried on GitHub.

## Layout

```
python/
  pyproject.toml        scikit-build-core; Python 3.10+; abi3 (cp312) wheel; pyarrow >= 15; the version; [tool.cibuildwheel]
  CMakeLists.txt        fstx_engine (static: src/ engine + vendored libs), _core (nanobind), coretest (option)
  LICENSE               fstx's MIT licence (full text; the R package's LICENSE is the CRAN two-line form)
  LICENSES/             copies of inst/COPYRIGHTS and src/vendor/<lib>/LICENSE, shipped in wheel and sdist
  tools/make_core.py    copies the engine into core/ (git-ignored) for the sdist, refreshes LICENSES/
  cmake/msvc_compat.h   cl.exe only, force-included (/FI): __builtin_ctzll / clzll, __attribute__ dropped
  src/binding.h         shared by the binding: File (holds fstx_file*), raise_error / check, init_* declarations
  src/module.cpp        NB_MODULE(_core): FstxError, ERR_* codes, build_info, set_simd, kernel_selftest, shutdown
  src/file.cpp          open, File (info, columns, column, levels, find_column, close)
  src/read.cpp          _core.stream (Arrow C stream capsule), _core.filter, OP_* and VERIFY_* constants
  fstx/__init__.py      public names; registers _core.shutdown with atexit
  fstx/_file.py         FileMeta, ColumnMeta, File, open(), metadata()
  fstx/_engine.py       LOCK: the one lock around the engine's worker pool (reads, filters, writes)
  fstx/_meta.py         the Arrow metadata keys; b64() / blob() for attribute blobs
  fstx/_fxa1.py         FXA1 attribute blobs: decode() and encode(), kind(), RVector
  fstx/_filters.py      filters -> engine predicates, as R's .term_pred()
  fstx/_read.py         read, to_batches, read_fstx, schema_of, row_names, to_pandas, to_polars, set_options
  fstx/_write.py        write_fstx
  tests/                conftest.py (golden sets, same()), test_metadata.py, test_build.py, test_read.py,
                        test_write.py, test_roundtrip.py, golden_tables.py, golden/
  DESIGN.md, DEV.md
```

Rules for the next steps:

- One `.cpp` per area in `python/src/` (`read.cpp`, `write.cpp`), each with `void init_<area>(nb::module_&)`
  declared in `binding.h`, called from `module.cpp`, listed in `nanobind_add_module` in `CMakeLists.txt`.
- The binding uses the engine through `fstx_core.h` and `arrow_abi.h` only (module.cpp also reads `simd.h` and
  `kern_test.h` for build information). Engine errors go through `fstxpy::check(rc, err)`.
- Engine objects made from a file (plans, column sources, Arrow streams opened by path are independent) need the
  `File` alive: keep a reference and use `File::get()`, which refuses a closed file.
- Release the GIL around engine calls that read or write (`nb::gil_scoped_release`); callbacks that the engine
  runs on worker threads (`get_strings`) must not touch Python objects.
- Stable ABI: the module is built with `Py_LIMITED_API` on 3.12+, so no CPython internals in the binding.
- Python code: one private module per area (`_file.py`, later `_read.py`, `_write.py`), public names re-exported
  in `__init__.py`. pyarrow is a dependency since the read step; pandas, polars and numpy are optional.

## API so far

```python
fstx.metadata(path) -> FileMeta   # path, nrow, ncol, row_groups, pages, profile, format_version, file_size,
                                  # row_group_size, table_attrs (raw blob), columns, .names
ColumnMeta                        # index, name, phys (integer / double / logical / character),
                                  # logical (none / factor / Date / POSIXct), ordered, levels, tzone
                                  # ("" = stored empty, None = absent), attrs (raw blob), compressed_bytes,
                                  # raw_bytes, pages
fstx.open(path) -> File           # context manager: num_rows, num_columns, shape, column_names, columns,
                                  # column(i | name), metadata, path, closed, close()
fstx.FstxError                    # args (message, code, column); .message .code .column; fstx.ERR_* codes
fstx.build_info()                 # engine version, compiler, abi3, SIMD targets, threads
fstx._core                        # private: open, File, set_simd, kernel_selftest, shutdown
```

Added by the read step:

```python
fstx.read_fstx(path, columns=None, rows=None, filters=None, *, threads=None, verify=True, as_factor=False,
               attributes=True, datetimes="arrow", invalid_utf8="replace") -> pyarrow.Table
File.read(columns, rows, filters, **same options)        File.schema
File.to_batches(columns, batch_rows, filters, *, rows, threads, verify, attributes, datetimes, invalid_utf8)
                                  # pyarrow.RecordBatchReader; File also has __arrow_c_stream__
FileMeta.schema  .attributes  .table_class  .row_names   ColumnMeta.r_class  .attributes
fstx.to_pandas(table)  fstx.to_polars(table)  fstx.set_options(verify_nulls=)  fstx.FstxWarning
fstx._fxa1.decode(blob) -> {name: RVector(type, values, attrs)}
```

## Changes to the engine sources (`src/`)

1. `src/core_writer.cpp`: `dbl_stats` and `int_stats` get a plain-loop version under
   `#if defined(_MSC_VER) && !defined(__clang__)`; the GCC / Clang vector-extension code is the `#else`, untouched.
2. `src/enc_num.cpp`: the same for `any_int_na` and `any_nan`.

Reason: cl.exe has no `__attribute__((vector_size))`. Results are the same (coretest hashes above).

3. (read step) `src/arrow_abi.h`: `fstx_arrow_args` gains a last member `int raw`; `src/r_arrow.cpp`: when it
   is set, Date and POSIXct columns leave the stream as stored (int32 / float64) instead of date32 /
   timestamp. Reason: `datetimes="float"` must return the exact stored doubles, which the timestamp
   conversion rounds. The R glue and `tools/fuzz/fuzz_arrow.cpp` zero the struct, so R never sets it.

Nothing else in `src/` changed; `.Rbuildignore` gained `^python$`.

Handled without source edits: `__builtin_ctzll` / `__builtin_clzll` (engine, vendor/fsst) and
`__attribute__((always_inline))` / `format` through `cmake/msvc_compat.h` for cl only; clang-cl needs none of
this and compiles the engine as it is. Under cl the two `always_inline` helpers in `enc_str.cpp` are plain
`inline` (speed not measured).

`r_arrow.cpp` (the Arrow C stream, `fstx_arrow_stream_open`) is in the engine library, compiled with
`FSTX_ARROW_NO_R`, which leaves its R glue out; it compiles under cl and clang-cl. Reading goes through it. `prof.cpp` is in too (no R code; counters compiled out).

## The `-Wa,-muse-unaligned-vector-move` flag

It exists for GCC on Win64 only (`configure.win`, `src/Makevars.win.in`): GCC bug 54412, GCC does not align
the Win64 stack to 32 bytes, so aligned AVX moves (`vmovdqa`) of 32-byte vectors kept on the stack can fault;
the flag makes the assembler write unaligned moves. cl and clang-cl align the stack themselves when a function
holds 32-byte vectors, so there is no equivalent and none is needed: the AVX2 kernels pass the self-test and
coretest under both. `CMakeLists.txt` adds the flag only for MinGW GCC on x86-64.

Floating point: files must be byte-identical on every CPU, so no fused multiply-add. GCC / Clang:
`-ffp-contract=off`; clang-cl: `/clang:-ffp-contract=off`; cl: `/fp:precise` plus `/fp:contract-` when accepted.

## Open points

- Which Windows compiler the wheels use: both pass. clang-cl compiles the same code paths as the R build
  (vector extensions, always_inline) and Highway recommends it over cl; cl is what pip and cibuildwheel use by
  default, so the wheels use cl now. No speed comparison was run. Wheels with clang-cl: `CC` and `CXX` set to
  `clang-cl` and `CMAKE_GENERATOR=Ninja` in a Windows `environment` under `[tool.cibuildwheel]`.
- `fstx_hardware_threads()` honours `_R_CHECK_LIMIT_CORES_` and `OMP_THREAD_LIMIT` in Python too.
- Build warnings were not reviewed one by one (lines with "warning" in the coretest build logs: 1 under cl, 6 under clang-cl).

## Writing: `write_fstx` (step 3, branch `py-write`)

Tested 17 Nov 2026 on the desktop (Windows x64, Python 3.12, pyarrow 26.0.1, pandas 3.1.1, polars 2.1.0), cl
and clang-cl: 260 tests pass under each (`tests/test_write.py` is new). R 4.5.1 on
the laptop then read the files: `check_roundtrip.R` 69 files = 57 PASS, 11 KNOWN, 1 FAIL (below);
`check_native.R` 23 files, 23 PASS. No engine (`src/`) change was needed.

Layout:

- `fstx/_write.py`: `write_fstx(data, path, *, profile=None, compress=None, level=None, threads=None,
  append=False, int64="auto")`. Every type decision is here, with pyarrow (DESIGN.md sections 3 to 5).
- The FXA1 encoder (`encode`, `Vec`, `logical` / `integer` / `double` / `character`) was `fstx/_fxa1_write.py`
  on this branch, with a private `parse`; since the merge both live in `fstx/_fxa1.py`.
- `src/write.cpp`: `_core.write(path, columns, table_attrs, nrow, codec, level, threads, profile, keep_pct,
  append)`. A column is `(name, phys, logical, ordered, levels, tzone, attrs, [Array.__arrow_c_array__()...])`.
  Number columns arrive as one int32 / float64 array with R's NA already filled in; string columns as their
  chunks (string, large_string, binary, large_binary). With the GIL released it first checks every string
  (UTF-8 for text, no NUL, at most 2^31-1 bytes, sound offsets; `ValueError` with column and 0-based row),
  then runs the engine's open / append / close. The string callback reads Arrow buffers only.
- `tests/golden_tables.py`: builds the values `read_fstx()` is to return from a golden sidecar, without the
  reader. On this branch it stood in for the reader (with a stand-in `to_pandas()`); since the merge the write
  tests use `fstx.read_fstx` and `fstx.to_pandas`, and it is a second opinion on the reader's values
  (`test_roundtrip.py`). `tests/golden/full/*.json` hold the values the sidecars cut short
  (`make_full_values.R`, checked against the sidecar md5).
- `tests/golden/check_native.R`: R reads the files made from plain Python data
  (`tests/golden/from_python/native/`) and compares them with the same tables built in R.
- `tests/golden/from_python/` is made by the tests and not committed.

Metadata contract: see "One contract for reading and writing" below. Still as written here:

- A `binary` column is written as text only when its `fstx.r` says phys character (`invalid_utf8="binary"`).
- The pandas index is the row names: RangeIndex 0..n-1 = none, distinct strings = character, distinct int32
  whole numbers = integer; anything else is refused (reset_index).
- Metadata that does not fit the Arrow type any more is ignored with a `UserWarning`.
- A zero-column table keeps its row count only as a table built from an empty struct array, with the schema
  metadata put on the record batch (`Table.replace_schema_metadata` loses the rows).

Engine use: the engine has one worker pool that runs one job at a time (two writes from two Python threads
gave `FstxError: worker pool is already running a job`). A lock makes writes take turns; since the merge it is
`fstx/_engine.py` `LOCK`, and the reader takes it too.

Open after this step (the first two were closed by the merge):

- Error rows are counted from 0.
- Two threads appending to the same file (fixed 18 Nov 2026 on Harsh's "do 1-5"): an append used to read the
  file's metadata before taking the engine lock, and appends failed at random with "file is in use by another
  process" (Windows error 32), the engine's "memory-mapped by a lazy read_fstx() result" check (worded for R), or
  "the new levels must start with the file's levels" (no data harm). Now `write_fstx(append=True)` plans
  (`_append_plan`: metadata, levels, profile), converts the columns and writes in one step under `LOCK`
  (re-entrant). A plain write still converts its columns outside the lock and takes it for `_core.write` only.
  Other processes appending to the same file are not covered (the lock is per process).
- Not tested: a string column past 2 GB, string_view input, float16, Linux / macOS, Python other than 3.12.

## One contract for reading and writing (merge of `py-wheels`, `py-write`, `py-read`, 17 Nov 2026)

The three branches are merged into `python`, and the reader and the writer now use the same metadata, the same
FXA1 module and the same engine lock. DESIGN.md section 4 is the contract; in short:

| where | key | value |
|---|---|---|
| field | `fstx.r` | JSON `{"phys","logical","class"?,"tzone"?}`; the reader puts it on every column, the writer takes a column without it as plain data |
| field | `fstx.attrs` | the column's attribute blob, base64 (the writer also takes raw bytes) |
| schema | `fstx.table_attrs` | the table's attribute blob, base64 (the writer also takes raw bytes) |
| schema | `fstx.nrow` | row count, decimal text; only on a whole-file read whose table blob is R serialization |
| pandas | `df.attrs["fstx"]` | `{"table_attrs", "nrow", "columns": [{"name", "r", "attrs"}]}`, blobs as base64 text; row names in the index |
| polars | - | nothing travels |

- Row names: only in the table blob. A partial or reordered read rewrites an FXA1 blob so that `row.names` are
  those of the rows read (`_read._with_row_names`); `fstx.row_names` is gone. `_read.row_names(schema)` decodes
  them from the blob.
- `fstx/_fxa1.py` is the one FXA1 module: `decode` (alias `parse`), `encode`, `RVector` (alias `Vec`), the four
  constructors, `kind`, `is_fxa1`. A double `None` is NA_real_ both ways, every other NaN keeps its payload;
  strings that are not UTF-8 decode with `surrogateescape` and encode back to the stored bytes; two RVectors
  are equal when they are stored as the same bytes.
- `fstx/_meta.py`: the four keys, `b64(blob)` and `blob(value)` (base64 as bytes or str, or the raw blob).
- `fstx/_engine.py`: `LOCK`, taken by a write, by `_core.filter`, by opening a read stream, by each batch pulled
  from it and by closing it. Between batches the lock is free, so a batch reader that is left open does not
  block a write. It is re-entrant: a batch reader collected by the garbage collector in a thread that holds the
  lock must be able to close its stream.
- `to_pandas`: nullable dtypes for logical, integer, integer64 and double columns; a factor with NA as a level
  becomes strings (warning) and loses its `fstx.r`.
- Tests: `test_write.py` reads the golden files with `fstx.read_fstx` and goes through the real
  `fstx.to_pandas` / `fstx.to_polars` (files `from_python/<case>.fstx`, `.pandas.fstx`, `.polars.fstx`);
  `test_roundtrip.py` is new: reader against the sidecars, golden file -> read -> write -> read (pyarrow,
  pandas, polars; also `datetimes="float"` with `invalid_utf8="binary"`, which gives back the stored column
  definitions), ASCII-only metadata, polars on R-serialized and non-ASCII blobs, raw and base64 blobs alike,
  partial reads written back (`from_python/native/rownames_*.fstx`, `classes_blob.part.fstx`, checked by
  `check_native.R`), reads, filters, batches, writes and appends from 8 threads at once, a batch reader left
  open during a write.
- `manifest.json`: `difftime` through pandas is now "not identical" (a pandas `Float64` column cannot tell NaN
  from NA, as `basic`); `roundtrip_polars` is new for every case; the `basic` and `factor_na_level` pandas notes
  say what `to_pandas` does now. `make_golden.R` writes the same manifest (not rerun: the golden files did not
  change).

Tested (17 Nov 2026):

- Desktop, Windows x64, Python 3.12, pyarrow 26.0.1, pandas 3.1.1, polars 2.1.0, wheel built with cl from the
  merged tree: 795 tests passed (`test_build.py`, `test_metadata.py`, `test_read.py`, `test_write.py`,
  `test_roundtrip.py`). The wheel was built before the last Python-only edits; for this run the final `.py`
  files were copied over the installed package (the compiled module was the merged one).
- Laptop, R 4.5.1 with fstx 1.0.0, on the files that run wrote: `check_roundtrip.R` 104 files = 73 PASS,
  31 KNOWN, 0 FAIL (pyarrow 30 PASS / 5 KNOWN, pandas 27 / 8, polars 16 / 18; `names` is not written through
  polars); `check_native.R` 31 files, 31 PASS.
- 18 Nov 2026, desktop, tree at b040c70 synced in full, every wheel built from an empty build folder:
  - cl (MSVC 19.44) wheel: 795 tests passed.
  - clang-cl (Clang 19.1.5) wheel: 795 tests passed.
  - coretest with cl and with clang-cl, 200,000 rows: 470 checks passed, 0 failed each; kernels scalar / AVX2 /
    SSE4 / SSE2 0 mismatches, same hashes for every target and both compilers.
  - R package built from the same tree (R 4.6.1), `test-arrow.R` through `run_tests.R`: 10 tests,
    356 expectations, 0 failed, 0 errors, 1 skipped. The skip is "arrow, DuckDB and polars take the stream"
    (`test-arrow.R:300`): it ends with `skip_if()` when none of the optional consumers arrow, duckdb, polars is
    installed, and the desktop R has none of them (it has nanoarrow, so the earlier tests run). By design, not
    a setup bug; nothing changed. The laptop R has arrow but neither nanoarrow nor testthat, so it was not run
    there either.
  The merge brought no change to `src/` beyond what `py-read` already had (`arrow_abi.h`, `r_arrow.cpp`).
- 18 Nov 2026, R-match part A (dates, times, integer64; section below), desktop, clang-cl wheel: `test_rmatch.py`
  34 passed. Laptop, R 4.5.1 with fstx 1.0.0: `check_rmatch.R` 21 files = 20 PASS, 1 OPEN
  (`native_fixed_offset.pyarrow`), 0 FAIL.
- 18 Nov 2026, R-match part B (factors, NA, strings), desktop, clang-cl wheel: `test_rmatch.py` 61 passed
  (part A included). Laptop R 4.5.1: `check_rmatch.R` 39 files = 38 PASS, 1 OPEN (the same fixed offset), 0 FAIL.
- 18 Nov 2026, two threads appending to one file, desktop, clang-cl wheel: `test_roundtrip.py` 221 passed, three
  runs in a row (two new tests; behaviour under "Open after this step" above). `scripts/append2.py` (20 rounds
  of 1,000 rows per thread): plain columns, thread 0 failed on its 2nd append (Windows error 32), 22,000 rows
  all present; factor levels, thread 1 failed on its 1st append (levels), 21,000 rows, 0 rows with a wrong level.
- 18 Nov 2026, end of the R-match / append / speed work: cl (MSVC 19.44) wheel rebuilt from the tree and
  installed, whole suite 858 passed. Speed check on that wheel: `README.md` "Speed" (scripts `tools/bench_py.py`,
  `tools/bench_r.R`, run through `scripts/bench.cmd`). Seen there, not looked into: Python's write of the
  10-million-row table takes 0.44 s where R's takes 0.15 s (1 thread: 1.36 s against 0.86 s), and reading 2 of
  10 columns (one a 20-value string column) takes 0.19 s of the full read's 0.24 s.
- 18 Nov 2026, task 1 (fixed-offset time zones), desktop, cl wheel with the new `.py` files copied over it
  (`scripts/pyinst.cmd`): whole suite 862 passed (`test_rmatch.py`: the old fixed-offset test replaced by
  5 tests: 8 offsets both signs, half and three-quarter hours, zero; append; filter; mapping; old-style file;
  pandas and polars). Laptop R 4.5.1: `check_rmatch.R` 44 files = 44 PASS, 0 OPEN, 0 FAIL.
- 18 Nov 2026, task 2 (two-thread append), desktop, same wheel with the new `.py` files: `scripts/append5.cmd`
  ran `test_two_threads_appending_to_one_file` (plain and factor levels, 2 threads x 20 appends) 10 times in a
  row: 2 passed each time, every append succeeded; `scripts/append2.py`: 0 errors, 41,000 rows, 0 rows with the
  other thread's level, 42 levels. Whole suite: 862 passed (one run stopped by the speech-test guard, rerun).
- 18 Nov 2026, task 3 (write speed), desktop, cl (MSVC 19.44) wheel rebuilt with the new `write.cpp`, final `.py`
  files copied over it: whole suite 866 passed (4 new in `test_write.py`: a bad string in a dictionary-coded
  column names its first row, three ways; a 20-value column across chunks with nulls is dictionary-coded and
  reads back). Cause found with a per-column profile (`scripts/prof_py.py`, `prof_r.R`): the engine's string
  dictionary keys on pointers, Arrow gives each row its own, so repeating strings from Python were never
  dictionary-coded (DESIGN.md section 6). Engine time of the bench write 0.37 -> 0.18 s (R 0.15). Bench, median
  of 5: write 0.44 -> 0.29 s, 1 thread 1.36 -> 1.20 s, 2-column read 0.19 -> 0.09 s, full read 0.24 s
  unchanged, file 224 -> 222.8 MiB (R's: 222.8). Left: about 0.1 s of serial Python preparation (casts, bool to
  int32, `pc.min`), and the dictionary decode on read (0.045 s for the 20-value column), both Python-side.
  clang-cl wheel not rebuilt for this step.
- 18 Nov 2026, task 4 (licence and packaging), desktop, `scripts/sdist.cmd` (make_core, then scikit-build-core
  1.1.2's `build_sdist`; the `build` package is not installed there): sdist 282 entries, 1.6 MB, with LICENSE,
  README.md, LICENSES/ (COPYRIGHTS + 6 libraries) and the engine in `core/` (146 files, 6 vendored libraries), no
  build output. PKG-INFO: Metadata-Version 2.4, `License-Expression: MIT`, 8 `License-File:` lines, README as
  text/markdown. The sdist pip-installed (built with cl from the unpacked copy) into a fresh venv with pyarrow
  26.0.1, pandas 3.1.1, polars 2.1.0, pytest 9.2.0: the wheel's `.dist-info/licenses/` holds the same 8 files;
  pytest on the sdist's own tests, run outside the tree on the installed package: 866 passed. `python/LICENSES`
  matches `inst/COPYRIGHTS` and `src/vendor/*/LICENSE`.
- Still open: Linux, macOS, Python other than 3.12.

## R-match tests (`test_rmatch.py`, 18 Nov 2026)

What Python shows is what R shows, both ways, value by value. `golden/rmatch/make_rmatch.R` (laptop: needs bit64
and jsonlite, which the desktop R lacks) writes each case as `<case>.fstx` plus `<case>.view.json`: R's view
from `rview.R` (per column typeof, class, tzone, levels, and every value as text R would show, POSIXct as wall
time and offset in its own zone). `tests/rview.py` builds the same view in Python from a pyarrow table, a
pandas or a polars DataFrame. `test_rmatch.py`:

- R -> Python: each case read with pyarrow, `fstx.to_pandas`, `fstx.to_polars`; views must equal R's apart from
  `KNOWN` (DESIGN.md section 8).
- R -> Python -> R and Python data -> R: written to `from_python/rmatch/<name>.fstx` with the view R is to show;
  read back here, then `check_rmatch.R` (laptop) reads each file in R and compares (PASS / OPEN / FAIL).

Part A cases: `rmatch_dates` (double and integer Dates, years 1 and 9999, NA), `rmatch_times` (UTC, +05:30,
New York across both DST changes, +05:45, St John's, tzone `""` and none, NA, fractions, year 9999),
`rmatch_microseconds`, `rmatch_int64` (past int32 and 2^53, the extremes, all NA, small values); native
pyarrow / pandas / polars times (s, ms, us, ns, naive, date32, date64) and int64 (`auto`, `integer64`,
`double`, uint32 / uint64, the refusals).

Part B cases: `rmatch_factors` (NA codes, unused levels, ordered, an NA level next to NA codes, the levels
`"NA"` and `""`, Indian-script levels, all NA, no levels), `rmatch_na` (NA in every type, NaN apart from NA,
`""` and `"NA"` apart from NA, all-NA columns of every type), `rmatch_strings` (Indian scripts, a 4-byte emoji,
combining marks, CJK, right-to-left, control characters, 100 KB and 380 KB strings); native pyarrow / pandas /
polars factors (dictionary index widths, large_string dictionaries, two chunks with different dictionaries,
pandas Categorical, polars Categorical and Enum), NA (null type, NaN, `""`, pandas object / str / nullable
dtypes) and strings (string, large_string, string_view, pandas str and object).

Found:

- polars: an Int64 whose values all fit int32 comes back integer (`int64="auto"`), added to DESIGN.md section 8.
- polars: a factor with an NA level comes back as strings (as pandas already did), added to DESIGN.md section 8.
- `rview.py` (test side only): an all-None object column in pandas is the writer's all-null logical column, and
  an all-NA Date column from `to_pandas` keeps its Date class through the metadata.
- Fixed-offset time zones (was OPEN; closed 18 Nov 2026 on Harsh's "do 1-5"): pyarrow's `"+05:30"` was stored as
  R's tzone and R read it as a POSIX TZ string (UTC-05:30), 11 hours off. Now stored as `"<+0530>-05:30"` and read
  back as `"+05:30"` (DESIGN.md section 3.4, `_meta.r_tzone` / `arrow_tz`); filters take a naive value in that
  offset; `fstx.to_polars` gives UTC with a warning (polars has no fixed offsets). Proof first, on both Rs:
  `scripts/tzprobe.R` (8 offsets, years 1900 to 9999): `format()`, `as.POSIXlt()$hour` / `$gmtoff` and R write ->
  read right on R 4.5.1 (laptop) and R 4.6.1 (desktop); every format warns "unknown timezone" (values right),
  so the POSIX form is used and the UTC fallback is not.
