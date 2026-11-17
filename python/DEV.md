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
  pyproject.toml        scikit-build-core; abi3 (cp312) wheel; nanobind 2 on Python 3.9, nanobind 3 on 3.10+
  CMakeLists.txt        fstx_engine (static: src/ engine + vendored libs), _core (nanobind), coretest (option)
  cmake/msvc_compat.h   cl.exe only, force-included (/FI): __builtin_ctzll / clzll, __attribute__ dropped
  src/binding.h         shared by the binding: File (holds fstx_file*), raise_error / check, init_* declarations
  src/module.cpp        NB_MODULE(_core): FstxError, ERR_* codes, build_info, set_simd, kernel_selftest, shutdown
  src/file.cpp          open, File (info, columns, column, levels, find_column, close)
  fstx/__init__.py      public names; registers _core.shutdown with atexit
  fstx/_file.py         FileMeta, ColumnMeta, File, open(), metadata()
  tests/                conftest.py (golden sets), test_metadata.py, test_build.py, golden/ (from py-design)
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
  in `__init__.py`. pyarrow is not a dependency yet; the read step adds it.

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

Against `DESIGN.md` section 2: `FileMeta.schema` (a pyarrow.Schema), `ColumnMeta.r_class` (needs the FXA1
decoder) and `File.schema / read / to_batches` are for the read step; `File.columns` holds the stored column
metadata meanwhile.

## Changes to the engine sources (`src/`)

1. `src/core_writer.cpp`: `dbl_stats` and `int_stats` get a plain-loop version under
   `#if defined(_MSC_VER) && !defined(__clang__)`; the GCC / Clang vector-extension code is the `#else`, untouched.
2. `src/enc_num.cpp`: the same for `any_int_na` and `any_nan`.

Reason: cl.exe has no `__attribute__((vector_size))`. Results are the same (coretest hashes above). Nothing
else in `src/` changed; `.Rbuildignore` gained `^python$`.

Handled without source edits: `__builtin_ctzll` / `__builtin_clzll` (engine, vendor/fsst) and
`__attribute__((always_inline))` / `format` through `cmake/msvc_compat.h` for cl only; clang-cl needs none of
this and compiles the engine as it is. Under cl the two `always_inline` helpers in `enc_str.cpp` are plain
`inline` (speed not measured).

`r_arrow.cpp` (the Arrow C stream, `fstx_arrow_stream_open`) is in the engine library, compiled with
`FSTX_ARROW_NO_R`, which leaves its R glue out; it compiles under cl and clang-cl. It is linked but not yet
called from Python. `prof.cpp` is in too (no R code; counters compiled out).

## The `-Wa,-muse-unaligned-vector-move` flag

It exists for GCC on Win64 only (`configure.win`, `src/Makevars.win.in`): GCC bug 54412, GCC does not align
the Win64 stack to 32 bytes, so aligned AVX moves (`vmovdqa`) of 32-byte vectors kept on the stack can fault;
the flag makes the assembler write unaligned moves. cl and clang-cl align the stack themselves when a function
holds 32-byte vectors, so there is no equivalent and none is needed: the AVX2 kernels pass the self-test and
coretest under both. `CMakeLists.txt` adds the flag only for MinGW GCC on x86-64.

Floating point: files must be byte-identical on every CPU, so no fused multiply-add. GCC / Clang:
`-ffp-contract=off`; clang-cl: `/clang:-ffp-contract=off`; cl: `/fp:precise` plus `/fp:contract-` when accepted.

## Open points

- Python 3.9: nanobind 3 needs 3.10. `pyproject.toml` pins nanobind 2 for 3.9, but that path was not built.
  3.9 is past end of life; dropping it would remove the pin.
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

## Writing: `write_fstx` (step 3, branch `py-write`)

Tested 17 Nov 2026 on the desktop (Windows x64, Python 3.12, pyarrow 26.0.1, pandas 3.1.1, polars 2.1.0), cl
and clang-cl: 260 tests pass under each (`tests/test_write.py` is new). R 4.5.1 on
the laptop then read the files: `check_roundtrip.R` 69 files = 57 PASS, 11 KNOWN, 1 FAIL (below);
`check_native.R` 23 files, 23 PASS. No engine (`src/`) change was needed.

Layout:

- `fstx/_write.py`: `write_fstx(data, path, *, profile=None, compress=None, level=None, threads=None,
  append=False, int64="auto")`. Every type decision is here, with pyarrow (DESIGN.md sections 3 to 5).
- `fstx/_fxa1_write.py`: the FXA1 encoder (`encode`, `Vec`, `logical` / `integer` / `double` / `character`)
  and a private `parse`, which the writer needs to change blobs (row names, data.table key and index, NA
  levels). The reader's decoder is its own file (`_fxa1.py`); the two can be joined when the branches meet.
- `src/write.cpp`: `_core.write(path, columns, table_attrs, nrow, codec, level, threads, profile, keep_pct,
  append)`. A column is `(name, phys, logical, ordered, levels, tzone, attrs, [Array.__arrow_c_array__()...])`.
  Number columns arrive as one int32 / float64 array with R's NA already filled in; string columns as their
  chunks (string, large_string, binary, large_binary). With the GIL released it first checks every string
  (UTF-8 for text, no NUL, at most 2^31-1 bytes, sound offsets; `ValueError` with column and 0-based row),
  then runs the engine's open / append / close. The string callback reads Arrow buffers only.
- `tests/golden_tables.py`: builds the table `read_fstx()` is to return from a golden sidecar, and a stand-in
  `to_pandas()`. The write tests use it because this branch has no reader. `tests/golden/full/*.json` hold the
  values the sidecars cut short (`make_full_values.R`, checked against the sidecar md5).
- `tests/golden/check_native.R`: R reads the files made from plain Python data
  (`tests/golden/from_python/native/`) and compares them with the same tables built in R.
- `tests/golden/from_python/` is made by the tests and not committed.

Metadata contract (the reader has to produce exactly this for a round trip):

- field `b"fstx.r"`: JSON `{"phys", "logical", "class", "tzone"}`: phys integer / double / logical /
  character; logical none / factor / Date / POSIXct; class = R's full class vector (absent for none); tzone
  only for a POSIXct column that has the attribute. Wanted on every column, and always next to `fstx.attrs`.
- field `b"fstx.attrs"`: the column's attribute blob, raw. Schema `b"fstx.table_attrs"`: the table's blob, raw.
  Schema `b"fstx.nrow"`: rows when read (decimal text); used only for a table blob in R serialization.
- A `binary` column is written as text only when its `fstx.r` says phys character (`invalid_utf8="binary"`).
- pandas: `df.attrs["fstx"] = {"table_attrs", "nrow", "columns": [{"name", "r", "attrs"}]}`, blobs as bytes or
  base64 text. The index is the row names: RangeIndex 0..n-1 = none, distinct strings = character, distinct
  int32 whole numbers = integer; anything else is refused (reset_index).
- Metadata that does not fit the Arrow type any more is ignored with a `UserWarning`.
- A zero-column table keeps its row count only as a table built from an empty struct array, with the schema
  metadata put on the record batch (`Table.replace_schema_metadata` loses the rows).

Engine use: the engine has one worker pool that runs one job at a time (two writes from two Python threads
gave `FstxError: worker pool is already running a job`). `_write._ENGINE_LOCK` makes writes take turns; the
reader has to take the same lock around its engine calls.

Open after this step:

- `check_roundtrip.R` FAIL: `difftime.pandas.fstx`, column `weeks`, row 3: NaN comes back NA. A pandas float
  column cannot tell NaN from NA (the same reason `basic.pandas.fstx` is KNOWN); the manifest expects
  "identical if kept as float" although the column holds a NaN. Either the manifest line changes or
  `to_pandas()` hands doubles over as the nullable `Float64` dtype.
- `pyarrow` is imported by `fstx/_write.py` but is not in `pyproject.toml` dependencies yet.
- Error rows are counted from 0. An append reads the file's metadata before taking the engine lock, so two
  threads appending to the same file at once are not protected from each other.
- Not tested: a string column past 2 GB, string_view input, float16, Linux / macOS, Python other than 3.12.
