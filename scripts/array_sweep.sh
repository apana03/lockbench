#!/usr/bin/env bash
# array_sweep.sh — Sweep arraybench across thread counts, locks, and lock counts.
# Usage: ./scripts/array_sweep.sh [seconds] [max_threads] [repeats]

set -euo pipefail

SECONDS_PER_RUN=${1:-3}
MAX_THREADS=${2:-$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 8)}
REPEATS=${3:-3}
BIN="./build/arraybench"
CSV="results/arraybench.csv"

PIN_FLAG=""
[ "$(uname)" = "Linux" ] && PIN_FLAG="--pin"

if [ ! -x "$BIN" ]; then
  echo "Build first: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
  exit 1
fi

mkdir -p results
rm -f "$CSV"

THREAD_LIST="1 2 4 8"
for t in $(seq 16 16 "$MAX_THREADS" 2>/dev/null); do
  THREAD_LIST="$THREAD_LIST $t"
done

LOCKS_EXCL="tas ttas cas ticket"
LOCKS_ALL="tas ttas cas ticket rw occ"

echo "=== Exclusive locks, 64 locks, thread sweep [repeats=$REPEATS] ==="
for lock in $LOCKS_EXCL; do
  for t in $THREAD_LIST; do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    for r in $(seq 1 "$REPEATS"); do
      $BIN --lock "$lock" --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 3 --num_locks 64 $PIN_FLAG --csv "$CSV"
    done
  done
done
echo ""

echo "=== RW/OCC locks, 64 locks, 80% read, thread sweep ==="
for lock in rw occ; do
  for t in $THREAD_LIST; do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    for r in $(seq 1 "$REPEATS"); do
      $BIN --lock "$lock" --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 3 --num_locks 64 --read_pct 80 $PIN_FLAG --csv "$CSV"
    done
  done
done
echo ""

echo "=== RW/OCC locks, 64 locks, 95% read, thread sweep ==="
for lock in rw occ; do
  for t in $THREAD_LIST; do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    for r in $(seq 1 "$REPEATS"); do
      $BIN --lock "$lock" --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 3 --num_locks 64 --read_pct 95 $PIN_FLAG --csv "$CSV"
    done
  done
done
echo ""

echo "=== Lock count sweep (4 threads) ==="
for lock in ttas cas ticket occ; do
  for nlocks in 1 4 16 64 256 1024; do
    for r in $(seq 1 "$REPEATS"); do
      $BIN --lock "$lock" --threads 4 --seconds "$SECONDS_PER_RUN" --warmup 3 --num_locks "$nlocks" $PIN_FLAG --csv "$CSV"
    done
  done
done
echo ""

echo "=== Critical section work sweep (4 threads, 64 locks) ==="
for lock in ttas cas ticket; do
  for work in 0 50 100 500 1000; do
    for r in $(seq 1 "$REPEATS"); do
      $BIN --lock "$lock" --threads 4 --seconds "$SECONDS_PER_RUN" --warmup 3 --num_locks 64 --cs_work "$work" $PIN_FLAG --csv "$CSV"
    done
  done
done
