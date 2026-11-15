#!/bin/bash
# build.sh <package_dir> <work_dir> [jobs]: builds the fstx libFuzzer targets and the seed maker with clang 18,
# ASan + UBSan (Linux / WSL). The engine (src/*.cpp without the R files, plus the vendored zstd, lz4, FSST and
# Highway) is compiled once with -fsanitize=fuzzer-no-link; each target links it with -fsanitize=fuzzer.
# Output: <work_dir>/bin/fuzz_<target>, <work_dir>/bin/mkseeds, <work_dir>/bin/craft. Needs clang-18 and libclang-rt-18-dev.
set -e
PKG=$(cd "$1" && pwd)
WORK=$2
JOBS=${3:-4}
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}
S=$PKG/src
F=$PKG/tools/fuzz
VER=$(sed -n 's/^Version:[[:space:]]*//p' "$PKG/DESCRIPTION" | tr -d '\r')
mkdir -p "$WORK/obj" "$WORK/bin"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
INC="-I$S -I$S/vendor/flatbuffers/include -I$S/vendor/fsst -I$S/vendor/highway"
DEFS="-DZSTD_DISABLE_ASM -DHWY_DISABLED_TARGETS=25956824 -DFSTX_VERSION=$VER"
CXXF="-std=c++17 -O1 -g -fno-omit-frame-pointer -ffp-contract=off $INC $DEFS $SAN"
CF="-O1 -g -fno-omit-frame-pointer $DEFS $SAN"
# engine sources: everything in src/ that has no R header, and the vendored libraries (same list as Makevars.in)
ENGINE=$(cd "$S" && ls *.cpp | grep -v -E '^(r_|cpp11)')
VC=$(cd "$S" && ls vendor/zstd/common/*.c vendor/zstd/compress/*.c vendor/zstd/decompress/*.c vendor/lz4/lz4.c)
VCPP="vendor/fsst/libfsst.cpp vendor/fsst/fsst_nosimd.cpp vendor/highway/hwy/targets.cc vendor/highway/hwy/abort.cc"
echo "fstx $VER, engine: $(echo $ENGINE | wc -w) files, $CXX"
compile() {  # <source relative to src>
  local o="$WORK/obj/$(echo "$1" | tr '/' '_').o"
  if [ "$S/$1" -nt "$o" ] || [ ! -f "$o" ]; then
    case "$1" in
      *.c) $CC $CF -fsanitize=fuzzer-no-link -c "$S/$1" -o "$o" ;;
      *) $CXX $CXXF -fsanitize=fuzzer-no-link -c "$S/$1" -o "$o" ;;
    esac
  fi
}
export -f compile
export S WORK CC CXX CF CXXF
# headers changed: rebuild everything (simplest safe rule)
NEWEST_H=$(ls -t "$S"/*.h | head -1)
if [ -f "$WORK/obj/.stamp" ] && [ "$NEWEST_H" -nt "$WORK/obj/.stamp" ]; then rm -f "$WORK"/obj/*.o; fi
printf '%s\n' $ENGINE $VC $VCPP | xargs -P "$JOBS" -I{} bash -c 'compile {}'
touch "$WORK/obj/.stamp"
rm -f "$WORK/libfstx_engine.a"
ar rcs "$WORK/libfstx_engine.a" "$WORK"/obj/*.o
for t in $(cd "$F" && ls fuzz_*.cpp | sed 's/\.cpp$//'); do
  # fuzz_arrow includes src/r_arrow.cpp built without R; only trees with the WS2 stream entry have that
  if [ "$t" = fuzz_arrow ] && ! grep -q FSTX_ARROW_NO_R "$S/r_arrow.cpp"; then echo "skip fuzz_arrow (no FSTX_ARROW_NO_R in r_arrow.cpp)"; rm -f "$WORK/bin/$t"; continue; fi
  $CXX $CXXF -I"$F" -fsanitize=fuzzer "$F/$t.cpp" "$WORK/libfstx_engine.a" -lpthread -o "$WORK/bin/$t" &
  while [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; do sleep 1; done
done
FNM=$($CXX -print-resource-dir)/lib/linux/libclang_rt.fuzzer_no_main-x86_64.a
for t in mkseeds craft; do
  $CXX $CXXF -I"$F" "$F/$t.cpp" "$WORK/libfstx_engine.a" "$FNM" -lpthread -o "$WORK/bin/$t" &
done
wait
ls "$WORK/bin"
echo BUILDDONE
