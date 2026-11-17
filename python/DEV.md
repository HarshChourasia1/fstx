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
  Delete `python/core/` after making the sdist: while it exists, builds in the tree use it and not your `src/` edits.
- Wheels: `.github/workflows/python-wheels.yml` (cibuildwheel), on a `py-v*` tag or by hand, never on plain pushes.
  Windows x64 (cl), Linux x86_64 and aarch64 (manylinux_2_28; aarch64 on the native `ubuntu-24.04-arm` runner),
  macOS x86_64 (`macos-15-intel`, 10.15+) and arm64 (`macos-15`, 11+). Per platform: a cp310 and a cp311 wheel
  and one cp312-abi3 wheel, which cibuildwheel also tests on 3.13 and 3.14. pytest runs on every wheel. The sdist
  job also unpacks the sdist away from the tree, builds a wheel from it and runs pytest. Artifacts only: the
  PyPI job is in the file as comments, not enabled.
- Licences: `python/LICENSE` (fstx, MIT) and `python/LICENSES/` go into the sdist and into the wheel's
  `.dist-info/licenses/` (`wheel.license-files`). The metadata is version 2.1, so there are no `License-File:` lines;
  that needs `project.license-files` (PEP 639) and scikit-build-core 0.11+ in `build-system.requires`.

Checked on 17 Nov 2026 (desktop): sdist (244 entries, engine in `core/`) unpacked outside the git tree built
`cp312-abi3-win_amd64` and `cp311-cp311-win_amd64` wheels with cl; pytest 132 passed on 3.12, 3.11 and with the
abi3 wheel on 3.14. The same sdist in WSL Ubuntu 24.04 (GCC 13.3) built `cp312-abi3-linux_x86_64`, 132 passed, AVX2
dispatch (a plain Linux wheel; auditwheel puts it at manylinux_2_38 because of the newer glibc there, the
manylinux_2_28 image builds the real one). Not run: the workflow itself, so macOS, Linux aarch64, manylinux,
cp310, cp313, abi3audit and delvewheel are first tried on GitHub.

## Layout

```
python/
  pyproject.toml        scikit-build-core; Python 3.10+; abi3 (cp312) wheel; the version; [tool.cibuildwheel]
  CMakeLists.txt        fstx_engine (static: src/ engine + vendored libs), _core (nanobind), coretest (option)
  LICENSE               fstx's MIT licence (full text; the R package's LICENSE is the CRAN two-line form)
  LICENSES/             copies of inst/COPYRIGHTS and src/vendor/<lib>/LICENSE, shipped in wheel and sdist
  tools/make_core.py    copies the engine into core/ (git-ignored) for the sdist, refreshes LICENSES/
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

- Which Windows compiler the wheels use: both pass. clang-cl compiles the same code paths as the R build
  (vector extensions, always_inline) and Highway recommends it over cl; cl is what pip and cibuildwheel use by
  default, so the wheels use cl now. No speed comparison was run. Wheels with clang-cl: `CC` and `CXX` set to
  `clang-cl` and `CMAKE_GENERATOR=Ninja` in a Windows `environment` under `[tool.cibuildwheel]`.
- `fstx_hardware_threads()` honours `_R_CHECK_LIMIT_CORES_` and `OMP_THREAD_LIMIT` in Python too.
- Build warnings were not reviewed one by one (lines with "warning" in the coretest build logs: 1 under cl, 6 under clang-cl).
