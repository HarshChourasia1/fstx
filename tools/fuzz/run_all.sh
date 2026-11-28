#!/bin/bash
# run_all.sh <package_dir> <seconds_per_target> [cores] [work_dir] [targets...]
# Builds the fuzz targets (build.sh), makes the seed corpus (mkseeds), then runs every built target at the same
# time, one per core (at most <cores>, default 8), each for <seconds_per_target> wall seconds with libFuzzer's fork
# mode so a crash is saved and the run goes on. Phase 3 (2 CPU-hours per target; 7500 s covers fork-mode overhead):
#   bash tools/fuzz/run_all.sh <pkg> 7500 5 <new work dir>   (9 targets on 5 cores: two rounds, about 4 h 10 min)
# The work dir must be new: the script refuses one that already has artifacts/ (old artifacts would be counted).
# Output in <work_dir> (default /root/fstx/fuzz): corpus/<t>/, artifacts/<t>/ (crash-*, oom-*, timeout-*, leak-*),
# logs/<t>.log, summary.csv (target, seconds, execs, cov, ft, corpus, crashes, ooms, timeouts, leaks).
# Linux / WSL only, clang 18 with libclang-rt-18-dev. Memory: 768 MB RSS per target (5 targets leave room in WSL's
# 7 GB; 8 nearly fill it); a single allocation over 512 MB is reported as an OOM artifact (that is how the R6-1
# memory bomb shows up). Leak detection is on: a leak is saved as leak-* and counted in summary.csv.
# Seeds: mkseeds output plus regress/* (damaged files that must stay clean). regress/known/* are open findings
# (R6-1..R6-4, v1/ws-6/requests.md), named <target>_<case> (craft_* go through any reader target, e.g. read); replay one with <work_dir>/bin/fuzz_<target> <file>.
# Move a file from known/ to regress/ once its fix lands.
set -e
PKG=$(cd "$1" && pwd)
SECS=${2:-900}
CORES=${3:-8}
WORK=${4:-/root/fstx/fuzz}
shift 4 2>/dev/null || shift $#
F=$PKG/tools/fuzz
[ "$CORES" -gt 8 ] && CORES=8
[ -e "$WORK/artifacts" ] && { echo "work dir $WORK already has artifacts/ from an earlier run: use a new work dir"; exit 5; }
FZ=$(clang++-18 -print-resource-dir)/lib/linux/libclang_rt.fuzzer-x86_64.a
[ -f "$FZ" ] || { echo "libFuzzer runtime missing ($FZ): apt-get install libclang-rt-18-dev"; exit 3; }
bash "$F/build.sh" "$PKG" "$WORK" "$CORES"
rm -rf "$WORK/seeds"
"$WORK/bin/mkseeds" "$WORK/seeds" > "$WORK/mkseeds.log" 2>&1 || { cat "$WORK/mkseeds.log"; exit 4; }
cp "$F"/regress/* "$WORK/seeds/" 2>/dev/null || true
# the damaged files of craft (fstx 1.2 cases included) are seeds too
"$WORK/bin/craft" "$WORK/craft" > "$WORK/craft.log" 2>&1 && cp "$WORK"/craft/seed/* "$WORK/seeds/" 2>/dev/null || true
echo "seeds: $(ls "$WORK/seeds" | wc -l)"
TARGETS="$*"
if [ -z "$TARGETS" ]; then
  for t in $(cd "$F" && ls fuzz_*.cpp | sed 's/^fuzz_//; s/\.cpp$//'); do
    [ -x "$WORK/bin/fuzz_$t" ] || { echo "skip $t (not built)"; continue; }
    TARGETS="$TARGETS $t"
  done
fi
mkdir -p "$WORK/logs"
export ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:allocator_may_return_null=1:symbolize=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
running=0
for t in $TARGETS; do
  mkdir -p "$WORK/corpus/$t" "$WORK/artifacts/$t"
  cp -n "$WORK"/seeds/* "$WORK/corpus/$t/" 2>/dev/null || true
  ( cd "$WORK/artifacts/$t" && "$WORK/bin/fuzz_$t" -fork=1 -ignore_crashes=1 -ignore_ooms=1 -ignore_timeouts=1 \
      -max_total_time="$SECS" -timeout=10 -rss_limit_mb=768 -malloc_limit_mb=512 -max_len=65536 -dict="$F/fstx.dict" \
      -artifact_prefix="$WORK/artifacts/$t/" "$WORK/corpus/$t" > "$WORK/logs/$t.log" 2>&1 ) &
  running=$((running + 1))
  if [ "$running" -ge "$CORES" ]; then wait -n || true; running=$((running - 1)); fi
done
wait || true
echo "target,seconds,execs,cov,ft,corpus,crashes,ooms,timeouts,leaks" > "$WORK/summary.csv"
for t in $TARGETS; do
  L="$WORK/logs/$t.log"
  last=$(grep -E '^#[0-9]+' "$L" | tail -1)
  execs=$(echo "$last" | sed -n 's/^#\([0-9]*\).*/\1/p')
  cov=$(echo "$last" | sed -n 's/.*cov: \([0-9]*\).*/\1/p')
  ft=$(echo "$last" | sed -n 's/.*ft: \([0-9]*\).*/\1/p')
  corp=$(ls "$WORK/corpus/$t" | wc -l)
  cr=$(ls "$WORK/artifacts/$t" | grep -c '^crash-' || true)
  oo=$(ls "$WORK/artifacts/$t" | grep -c '^oom-' || true)
  to=$(ls "$WORK/artifacts/$t" | grep -c '^timeout-' || true)
  lk=$(ls "$WORK/artifacts/$t" | grep -c '^leak-' || true)
  echo "$t,$SECS,${execs:-0},${cov:-0},${ft:-0},$corp,$cr,$oo,$to,$lk" >> "$WORK/summary.csv"
done
cat "$WORK/summary.csv"
echo RUNDONE
