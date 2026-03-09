#!/usr/bin/env bash
# sweep.sh — Run lockbench across thread counts and lock types.
# Usage: ./scripts/sweep.sh [seconds] [max_threads]
#
# Output is TSV-friendly for easy import into spreadsheets / plotting.

set -euo pipefail

SECONDS_PER_RUN=${1:-3}
MAX_THREADS=${2:-$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 8)}
BIN="./build/lockbench"

if [ ! -x "$BIN" ]; then
  echo "Build first: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
  exit 1
fi

echo "=== Mutex workload (exclusive lock/unlock) ==="
for lock in tas ttas cas ticket rw occ; do
  for t in 1 2 4 8 $(seq 16 16 "$MAX_THREADS"); do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    $BIN --lock "$lock" --workload mutex --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 1
  done
  echo ""
done

echo "=== RW workload (reader-writer, 80% reads) ==="
for lock in rw occ; do
  for t in 1 2 4 8 $(seq 16 16 "$MAX_THREADS"); do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    $BIN --lock "$lock" --workload rw --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 1 --read_pct 80
  done
  echo ""
done

echo "=== RW workload (reader-writer, 95% reads) ==="
for lock in rw occ; do
  for t in 1 2 4 8 $(seq 16 16 "$MAX_THREADS"); do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    $BIN --lock "$lock" --workload rw --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 1 --read_pct 95
  done
  echo ""
done

echo "=== RCU workload (90% reads) ==="
for t in 1 2 4 8 $(seq 16 16 "$MAX_THREADS"); do
  [ "$t" -gt "$MAX_THREADS" ] && continue
  $BIN --lock rcu --workload rcu --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 1 --read_pct 90
done

echo ""
echo "=== Critical section cost sweep (4 threads, TTAS) ==="
for ns in 0 50 100 500 1000 5000; do
  $BIN --lock ttas --workload mutex --threads 4 --seconds "$SECONDS_PER_RUN" --warmup 1 --cs_ns "$ns"
done
