#!/usr/bin/env bash
# skiplist_sweep.sh - Sweep skiplistbench across thread counts, locks,
# distributions, zipfian thetas, and R/W mixes.
#
# Usage: ./scripts/skiplist_sweep.sh [seconds] [max_threads] [repeats]
#
# Workload matrix per run:
#   6 locks  × (#threads)  × 4 R/W mixes × {uniform + 4 zipfian thetas} × repeats
#
# Example runtime (defaults: 3s, 8 threads, 3 repeats):
#   uniform    : 4 mix × 6 lock × 5 thread × 3 repeat = 360 runs
#   zipfian    : 4 theta × 4 mix × 6 lock × 5 thread × 3 repeat = 1440 runs
#   total      : 1800 runs × ~6s = ~3 hours
# Reduce by shrinking THETA_LIST or MIX_LIST at the top of this file.

set -euo pipefail

SECONDS_PER_RUN=${1:-3}
MAX_THREADS=${2:-$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 8)}
REPEATS=${3:-3}
BIN="./build/skiplistbench"
OUT_DIR="results/skiplistbench"
CSV="$OUT_DIR/skiplistbench.csv"

PIN_FLAG=""
[ "$(uname)" = "Linux" ] && PIN_FLAG="--pin"

if [ ! -x "$BIN" ]; then
  echo "Build first: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
  exit 1
fi

mkdir -p "$OUT_DIR"
rm -f "$CSV"

THREAD_LIST="1 2 4 8"
for t in $(seq 16 16 "$MAX_THREADS" 2>/dev/null); do
  THREAD_LIST="$THREAD_LIST $t"
done

LOCKS="tas ttas cas ticket rw occ"

# R/W mixes as "read_pct:insert_pct" (delete_pct = 100 - read - insert)
MIX_LIST="95:4 80:10 50:25 20:50"

# Zipfian skew values (higher = more concentrated on hot keys)
THETA_LIST="0.5 0.7 0.9 0.99"

run_one() {
  local dist="$1" theta="$2" read_pct="$3" insert_pct="$4" lock="$5" threads="$6"
  $BIN --lock "$lock" --dist "$dist" --zipf_theta "$theta" \
       --threads "$threads" --seconds "$SECONDS_PER_RUN" --warmup 3 \
       --read_pct "$read_pct" --insert_pct "$insert_pct" \
       $PIN_FLAG --csv "$CSV"
}

echo "=== Uniform × R/W mix sweep [repeats=$REPEATS] ==="
for mix in $MIX_LIST; do
  read_pct="${mix%:*}"; insert_pct="${mix#*:}"
  echo "--- uniform ${read_pct}/${insert_pct}/$((100 - read_pct - insert_pct)) ---"
  for lock in $LOCKS; do
    for t in $THREAD_LIST; do
      [ "$t" -gt "$MAX_THREADS" ] && continue
      for r in $(seq 1 "$REPEATS"); do
        run_one uniform 0.0 "$read_pct" "$insert_pct" "$lock" "$t"
      done
    done
  done
done
echo ""

echo "=== Zipfian × theta × R/W mix sweep [repeats=$REPEATS] ==="
for theta in $THETA_LIST; do
  for mix in $MIX_LIST; do
    read_pct="${mix%:*}"; insert_pct="${mix#*:}"
    echo "--- zipfian theta=$theta ${read_pct}/${insert_pct}/$((100 - read_pct - insert_pct)) ---"
    for lock in $LOCKS; do
      for t in $THREAD_LIST; do
        [ "$t" -gt "$MAX_THREADS" ] && continue
        for r in $(seq 1 "$REPEATS"); do
          run_one zipfian "$theta" "$read_pct" "$insert_pct" "$lock" "$t"
        done
      done
    done
  done
done
