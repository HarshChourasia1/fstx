# fstx for Python: the FST-X engine (../src, no R code) plus the nanobind module fstx._core.
# Built through scikit-build-core (pip install ./python); DEV.md has the commands.
cmake_minimum_required(VERSION 3.21)
project(fstx_python LANGUAGES C CXX)

# ---------------------------------------------------------------- where the engine is
# In the git tree it is ../src. An sdist can carry its own copy in python/core (DEV.md, wheels).
set(FSTX_CORE_DIR "" CACHE PATH "folder with the fstx engine sources (default: ../src)")
if(NOT FSTX_CORE_DIR)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/core/fstx_core.h")
    set(FSTX_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/core")
  else()
    set(FSTX_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src")
  endif()
endif()
get_filename_component(FSTX_CORE_DIR "${FSTX_CORE_DIR}" ABSOLUTE)
if(NOT EXISTS "${FSTX_CORE_DIR}/fstx_core.h")
  message(FATAL_ERROR "fstx engine sources not found in ${FSTX_CORE_DIR}")
endif()

# The engine version written into every file's created_by: DESCRIPTION is the one source, as in ./configure.
set(FSTX_CORE_VERSION "" CACHE STRING "engine version (default: read from DESCRIPTION)")
if(NOT FSTX_CORE_VERSION)
  foreach(d "${FSTX_CORE_DIR}/../DESCRIPTION" "${FSTX_CORE_DIR}/DESCRIPTION")
    if(NOT FSTX_CORE_VERSION AND EXISTS "${d}")
      file(STRINGS "${d}" _ver REGEX "^Version:")
      string(REGEX REPLACE "^Version:[ \t]*([^ \t\r]+).*$" "\\1" FSTX_CORE_VERSION "${_ver}")
    endif()
  endforeach()
endif()
if(NOT FSTX_CORE_VERSION)
  message(FATAL_ERROR "cannot find the engine version (DESCRIPTION); pass -DFSTX_CORE_VERSION=x.y.z")
endif()
message(STATUS "fstx engine ${FSTX_CORE_VERSION} from ${FSTX_CORE_DIR}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------- the engine: a static library, no R glue
# The list is OBJECTS of src/Makevars.in minus the R glue (r_*.cpp, cpp11.cpp). prof.cpp has no R code (its counters
# are compiled out unless FSTX_PROF is defined) and the engine calls into it, so it stays.
set(V "${FSTX_CORE_DIR}/vendor")
set(FSTX_VENDOR_SOURCES
  ${V}/zstd/common/debug.c ${V}/zstd/common/entropy_common.c ${V}/zstd/common/error_private.c
  ${V}/zstd/common/fse_decompress.c ${V}/zstd/common/pool.c ${V}/zstd/common/threading.c
  ${V}/zstd/common/xxhash.c ${V}/zstd/common/zstd_common.c
  ${V}/zstd/compress/fse_compress.c ${V}/zstd/compress/hist.c ${V}/zstd/compress/huf_compress.c
  ${V}/zstd/compress/zstd_compress.c ${V}/zstd/compress/zstd_compress_literals.c
  ${V}/zstd/compress/zstd_compress_sequences.c ${V}/zstd/compress/zstd_compress_superblock.c
  ${V}/zstd/compress/zstd_double_fast.c ${V}/zstd/compress/zstd_fast.c ${V}/zstd/compress/zstd_lazy.c
  ${V}/zstd/compress/zstd_ldm.c ${V}/zstd/compress/zstd_opt.c ${V}/zstd/compress/zstd_preSplit.c
  ${V}/zstd/compress/zstdmt_compress.c
  ${V}/zstd/decompress/huf_decompress.c ${V}/zstd/decompress/zstd_ddict.c
  ${V}/zstd/decompress/zstd_decompress.c ${V}/zstd/decompress/zstd_decompress_block.c
  ${V}/lz4/lz4.c
  ${V}/fsst/libfsst.cpp ${V}/fsst/fsst_nosimd.cpp
  ${V}/highway/hwy/targets.cc ${V}/highway/hwy/abort.cc)
set(FSTX_ENGINE_SOURCES
  ${FSTX_CORE_DIR}/core_util.cpp ${FSTX_CORE_DIR}/core_writer.cpp ${FSTX_CORE_DIR}/core_reader.cpp
  ${FSTX_CORE_DIR}/core_filter.cpp ${FSTX_CORE_DIR}/core_colsrc.cpp
  ${FSTX_CORE_DIR}/enc_num.cpp ${FSTX_CORE_DIR}/enc_str.cpp
  ${FSTX_CORE_DIR}/codec_ext.cpp ${FSTX_CORE_DIR}/simd_kernels.cpp ${FSTX_CORE_DIR}/kern_test.cpp
  ${FSTX_CORE_DIR}/prof.cpp
  ${FSTX_CORE_DIR}/r_arrow.cpp)
# r_arrow.cpp is the Arrow C stream (fstx_arrow_stream_open, arrow_abi.h) with R glue at its end; the glue is
# compiled out with FSTX_ARROW_NO_R, as tools/fuzz does. It is the one r_*.cpp file in this build.
set_source_files_properties(${FSTX_CORE_DIR}/r_arrow.cpp PROPERTIES COMPILE_DEFINITIONS FSTX_ARROW_NO_R)

add_library(fstx_engine STATIC ${FSTX_ENGINE_SOURCES} ${FSTX_VENDOR_SOURCES})
target_include_directories(fstx_engine PUBLIC
  "${FSTX_CORE_DIR}" "${V}/flatbuffers/include" "${V}/fsst" "${V}/highway")
# The preprocessor flags of PKG_CPPFLAGS in src/Makevars.in. 25956824 = the Highway targets left out
# (x86: SSSE3 and every AVX-512 flavour, so SSE2 / SSE4 / AVX2 stay; ARM: the SVE family, NEON stays).
target_compile_definitions(fstx_engine PUBLIC
  ZSTD_DISABLE_ASM
  HWY_DISABLED_TARGETS=25956824
  FSTX_VERSION=${FSTX_CORE_VERSION})

set(FSTX_IS_MSVC_CL OFF)   # cl.exe itself
set(FSTX_IS_CLANG_CL OFF)  # clang behind the MSVC command line
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(FSTX_IS_CLANG_CL ON)
elseif(MSVC)
  set(FSTX_IS_MSVC_CL ON)
endif()

# No fused multiply-add anywhere, so SIMD and plain code round the same and files are byte-identical on
# every CPU (what ./configure does with -ffp-contract=off).
include(CheckCXXCompilerFlag)
if(FSTX_IS_MSVC_CL)
  # /fp:precise is cl's default and VS 2022 does not contract under it; /fp:contract- says so out loud.
  target_compile_options(fstx_engine PRIVATE /fp:precise /bigobj)
  check_cxx_compiler_flag("/fp:contract-" FSTX_HAS_FP_CONTRACT_OFF)
  if(FSTX_HAS_FP_CONTRACT_OFF)
    target_compile_options(fstx_engine PRIVATE /fp:contract-)
  endif()
  # cl.exe has no __builtin_ctzll & co and no __attribute__: cmake/msvc_compat.h supplies them, included
  # ahead of every engine and vendor source (and of whatever links the engine and includes its headers).
  target_compile_options(fstx_engine PUBLIC "/FI${CMAKE_CURRENT_SOURCE_DIR}/cmake/msvc_compat.h" /utf-8)
  target_compile_definitions(fstx_engine PUBLIC _CRT_SECURE_NO_WARNINGS)
elseif(FSTX_IS_CLANG_CL)
  target_compile_options(fstx_engine PRIVATE /clang:-ffp-contract=off)
  target_compile_options(fstx_engine PUBLIC /utf-8)
  target_compile_definitions(fstx_engine PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
  check_cxx_compiler_flag("-ffp-contract=off" FSTX_HAS_FP_CONTRACT_OFF)
  if(FSTX_HAS_FP_CONTRACT_OFF)
    target_compile_options(fstx_engine PRIVATE -ffp-contract=off)
  else()
    message(WARNING "the compiler does not take -ffp-contract=off: files may differ between CPUs")
  endif()
  # GCC on Win64 (MinGW) only: GCC bug 54412, the stack is not aligned for 32-byte vectors, so the
  # assembler is told to write unaligned moves (configure.win). cl and clang align the stack themselves.
  if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    target_compile_options(fstx_engine PRIVATE -Wa,-muse-unaligned-vector-move)
  endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(fstx_engine PUBLIC Threads::Threads)

# ---------------------------------------------------------------- coretest (optional): the engine's own test program
option(FSTX_BUILD_CORETEST "build tools/coretest (engine self-test, no Python)" OFF)
if(FSTX_BUILD_CORETEST)
  add_executable(coretest "${FSTX_CORE_DIR}/../tools/coretest/coretest.cpp")
  target_link_libraries(coretest PRIVATE fstx_engine)
endif()

# ---------------------------------------------------------------- the Python module fstx._core
option(FSTX_BUILD_PYTHON "build the Python extension fstx._core" ON)
if(FSTX_BUILD_PYTHON)
  if(CMAKE_VERSION VERSION_LESS 3.26)
    find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)
  else()
    find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module
                 OPTIONAL_COMPONENTS Development.SABIModule)
  endif()
  if(NOT nanobind_DIR AND NOT nanobind_ROOT)
    execute_process(COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
                    OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE nanobind_ROOT)
  endif()
  find_package(nanobind CONFIG REQUIRED)

  # One .cpp per area, each with an init_<area>(nb::module_&) called from module.cpp (see src/binding.h).
  # STABLE_ABI: abi3 on CPython 3.12+, an ordinary module on 3.10 and 3.11.
  nanobind_add_module(_core STABLE_ABI NB_STATIC
    src/module.cpp
    src/file.cpp
    src/write.cpp)
  target_link_libraries(_core PRIVATE fstx_engine)
  target_compile_definitions(_core PRIVATE
    FSTX_PY_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
  install(TARGETS _core LIBRARY DESTINATION fstx RUNTIME DESTINATION fstx)
endif()
