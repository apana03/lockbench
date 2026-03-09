#!/usr/bin/env bash
# index_sweep.sh — Sweep indexbench across thread counts, locks, and distributions.
# Usage: ./scripts/index_sweep.sh [seconds] [max_threads]

set -euo pipefail

SECONDS_PER_RUN=${1:-3}
MAX_THREADS=${2:-$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 8)}
BIN="./build/indexbench"

if [ ! -x "$BIN" ]; then
  echo "Build first: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
  exit 1
fi

THREAD_LIST="1 2 4 8"
for t in $(seq 16 16 "$MAX_THREADS"); do
  THREAD_LIST="$THREAD_LIST $t"
done

LOCKS="tas ttas cas ticket rw occ"

echo "=== Uniform, 80% read / 10% insert / 10% delete ==="
for lock in $LOCKS; do
  for t in $THREAD_LIST; do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    $BIN --lock "$lock" --dist uniform --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 1 --read_pct 80 --insert_pct 10
  done
done
echo ""

echo "=== Zipfian (theta=0.99), 80/10/10 ==="
for lock in $LOCKS; do
  for t in $THREAD_LIST; do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    $BIN --lock "$lock" --dist zipfian --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 1 --read_pct 80 --insert_pct 10
  done
done
echo ""

echo "=== Read-heavy: 95% read / 4% insert / 1% delete, Uniform ==="
for lock in $LOCKS; do
  for t in $THREAD_LIST; do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    $BIN --lock "$lock" --dist uniform --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 1 --read_pct 95 --insert_pct 4
  done
done
echo ""

echo "=== Write-heavy: 20% read / 50% insert / 30% delete, Zipfian ==="
for lock in $LOCKS; do
  for t in $THREAD_LIST; do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    $BIN --lock "$lock" --dist zipfian --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 1 --read_pct 20 --insert_pct 50
  done
done
echo ""

echo "=== Contention sweep: varying bucket count (4 threads, uniform, 80/10/10) ==="
for lock in ttas cas ticket; do
  for buckets in 256 1024 4096 16384 65536 262144; do
    $BIN --lock "$lock" --dist uniform --threads 4 --seconds "$SECONDS_PER_RUN" --warmup 1 --read_pct 80 --insert_pct 10 --buckets "$buckets"
  done
done
