#!/usr/bin/env bash
# sweep.sh — Run lockbench across thread counts and lock types.
# Usage: ./scripts/sweep.sh [seconds] [max_threads] [repeats]
#
# Output is TSV-friendly for easy import into spreadsheets / plotting.

set -euo pipefail

SECONDS_PER_RUN=${1:-3}
MAX_THREADS=${2:-$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 8)}
REPEATS=${3:-5}
BIN="./build/lockbench"
CSV="results/lockbench.csv"

# auto-detect platform and pass --pin on Linux
PIN_FLAG=""
[ "$(uname)" = "Linux" ] && PIN_FLAG="--pin"

if [ ! -x "$BIN" ]; then
  echo "Build first: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
  exit 1
fi

mkdir -p results
rm -f "$CSV"

echo "=== Mutex workload (exclusive lock/unlock) [repeats=$REPEATS] ==="
for lock in tas ttas cas ticket rw occ; do
  for t in 1 2 4 6 8 $(seq 16 16 "$MAX_THREADS" 2>/dev/null); do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    for r in $(seq 1 "$REPEATS"); do
      $BIN --lock "$lock" --workload mutex --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 3 $PIN_FLAG --csv "$CSV"
    done
  done
  echo ""
done

echo "=== RW workload (reader-writer, 80% reads) ==="
for lock in rw occ; do
  for t in 1 2 4 6 8 $(seq 16 16 "$MAX_THREADS" 2>/dev/null); do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    for r in $(seq 1 "$REPEATS"); do
      $BIN --lock "$lock" --workload rw --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 3 --read_pct 80 $PIN_FLAG --csv "$CSV"
    done
  done
  echo ""
done

echo "=== RW workload (reader-writer, 95% reads) ==="
for lock in rw occ; do
  for t in 1 2 4 6 8 $(seq 16 16 "$MAX_THREADS" 2>/dev/null); do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    for r in $(seq 1 "$REPEATS"); do
      $BIN --lock "$lock" --workload rw --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 3 --read_pct 95 $PIN_FLAG --csv "$CSV"
    done
  done
  echo ""
done

echo "=== RCU workload (90% reads) ==="
for t in 1 2 4 6 8 $(seq 16 16 "$MAX_THREADS" 2>/dev/null); do
  [ "$t" -gt "$MAX_THREADS" ] && continue
  for r in $(seq 1 "$REPEATS"); do
    $BIN --lock rcu --workload rcu --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 3 --read_pct 90 $PIN_FLAG --csv "$CSV"
  done
done

echo ""
echo "=== Critical section cost sweep (4 threads, TTAS) ==="
for work in 0 50 100 500 1000 5000; do
  for r in $(seq 1 "$REPEATS"); do
    $BIN --lock ttas --workload mutex --threads 4 --seconds "$SECONDS_PER_RUN" --warmup 3 --cs_work "$work" $PIN_FLAG --csv "$CSV"
  done
done
