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

Not built or tested: Linux, macOS, ARM, Python other than 3.12 (3.9 to 3.11 take the non-abi3 path, and 3.9
needs nanobind 2: neither was run), the abi3 wheel on a newer Python, an sdist, `R CMD build` after the
`.Rbuildignore` change (the change is one line, `^python$`; the two engine edits are behind
`_MSC_VER && !__clang__`).

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

Metadata on what is returned (the writer reads this back):

- field `fstx.r`: JSON `{"phys","logical","class"?,"tzone"?}`, on every column that is not a plain vector
  without attributes. `class` is the R class vector (absent when plain or unknown); `tzone` only for a native
  POSIXct with a stored zone (`""` = stored empty, key absent = none stored).
- field `fstx.attrs`: the column's attribute blob, raw bytes. schema `fstx.table_attrs`: the table blob, raw.
- schema `fstx.row_names`: a JSON list, only on a partial or reordered read of a file with stored row names
  (it replaces the blob's `row.names`; repeats are made unique as R's `make.unique`). `row_names(schema)` in
  `_read.py` gives the row names either way.
- `to_pandas`: the schema, serialized, in `df.attrs["fstx_schema"]` (`pyarrow.ipc.read_schema`); row names in
  the index. `to_polars`: no metadata at all.

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

## Layout

```
python/
  pyproject.toml        scikit-build-core; abi3 (cp312) wheel; Python >= 3.10, nanobind >= 2.4, pyarrow >= 15
  CMakeLists.txt        fstx_engine (static: src/ engine + vendored libs), _core (nanobind), coretest (option)
  cmake/msvc_compat.h   cl.exe only, force-included (/FI): __builtin_ctzll / clzll, __attribute__ dropped
  src/binding.h         shared by the binding: File (holds fstx_file*), raise_error / check, init_* declarations
  src/module.cpp        NB_MODULE(_core): FstxError, ERR_* codes, build_info, set_simd, kernel_selftest, shutdown
  src/file.cpp          open, File (info, columns, column, levels, find_column, close)
  src/read.cpp          _core.stream (Arrow C stream capsule), _core.filter, OP_* and VERIFY_* constants
  fstx/__init__.py      public names; registers _core.shutdown with atexit
  fstx/_file.py         FileMeta, ColumnMeta, File, open(), metadata()
  fstx/_fxa1.py         FXA1 attribute blobs: decode(), kind(), RVector (the encoder is the write step's)
  fstx/_filters.py      filters -> engine predicates, as R's .term_pred()
  fstx/_read.py         read, to_batches, read_fstx, schema_of, row_names, to_pandas, to_polars, set_options
  tests/                conftest.py (golden sets), test_metadata.py, test_build.py, test_read.py, golden/
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

- Python 3.9 was dropped in the read step (floor 3.10, no nanobind 2 pin).
- The attribute blobs are raw bytes in the Arrow metadata (DESIGN.md section 4). polars panics on metadata
  that is not UTF-8 (`pl.from_arrow(table)` on a table whose blob is R serialization, or FXA1 with bytes over
  0x7F); `fstx.to_polars` drops the metadata for that reason. Storing the blobs as base64 would avoid it, at
  the cost of changing the contract in DESIGN.md.
- Which Windows compiler the wheels use: both pass. clang-cl compiles the same code paths as the R build
  (vector extensions, always_inline) and Highway recommends it over cl; cl is what pip and cibuildwheel use by
  default. No speed comparison was run.
- sdist: `CMakeLists.txt` reads the engine from `../src` (and the version from `../DESCRIPTION`), which an
  sdist made inside `python/` does not contain. It already accepts a copy in `python/core/` (with `DESCRIPTION`
  in it, or `-DFSTX_CORE_VERSION=`); the wheels step has to add the copy step. Licence files of the vendored
  libraries (`inst/COPYRIGHTS`, `src/vendor/*/LICENSE`) are not in the wheel yet.
- The package version is a fixed `1.0.0.dev0` in `pyproject.toml`; the engine version comes from `DESCRIPTION`.
- `fstx_hardware_threads()` honours `_R_CHECK_LIMIT_CORES_` and `OMP_THREAD_LIMIT` in Python too.
- Build warnings were not reviewed one by one (lines with "warning" in the coretest build logs: 1 under cl, 6 under clang-cl).
