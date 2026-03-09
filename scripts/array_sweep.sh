#!/usr/bin/env bash
# array_sweep.sh — Sweep arraybench across thread counts, locks, and modes.
# Usage: ./scripts/array_sweep.sh [seconds] [max_threads]

set -euo pipefail

SECONDS_PER_RUN=${1:-3}
MAX_THREADS=${2:-$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 8)}
BIN="./build/arraybench"

if [ ! -x "$BIN" ]; then
  echo "Build first: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
  exit 1
fi

THREAD_LIST="1 2 4 8"
for t in $(seq 16 16 "$MAX_THREADS"); do
  THREAD_LIST="$THREAD_LIST $t"
done

LOCKS="tas ttas cas ticket rw occ"

echo "=== Single Lock, 80% read, scan_len=16 ==="
for lock in $LOCKS; do
  for t in $THREAD_LIST; do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    $BIN --lock "$lock" --mode single --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 1 --read_pct 80 --scan_len 16
  done
done
echo ""

echo "=== Single Lock, 95% read, scan_len=16 ==="
for lock in $LOCKS; do
  for t in $THREAD_LIST; do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    $BIN --lock "$lock" --mode single --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 1 --read_pct 95 --scan_len 16
  done
done
echo ""

echo "=== Single Lock, 50% read (write-heavy), scan_len=16 ==="
for lock in $LOCKS; do
  for t in $THREAD_LIST; do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    $BIN --lock "$lock" --mode single --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 1 --read_pct 50 --scan_len 16
  done
done
echo ""

echo "=== Striped (64 stripes), 80% read, scan_len=16 ==="
for lock in $LOCKS; do
  for t in $THREAD_LIST; do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    $BIN --lock "$lock" --mode striped --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 1 --read_pct 80 --scan_len 16 --stripes 64
  done
done
echo ""

echo "=== Stripe count sweep (4 threads, 80% read) ==="
for lock in ttas cas ticket occ; do
  for stripes in 1 4 16 64 256 1024; do
    $BIN --lock "$lock" --mode striped --threads 4 --seconds "$SECONDS_PER_RUN" --warmup 1 --read_pct 80 --scan_len 16 --stripes "$stripes"
  done
done
