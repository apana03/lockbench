#!/usr/bin/env bash
# cds_sweep.sh — Sweep cdsbench (libcds StripedMap) across thread counts,
# locks, and distributions. Mirrors index_sweep.sh but targets the
# vendored libcds StripedMap with pluggable per-stripe lock primitive.
# Usage: ./scripts/cds_sweep.sh [seconds] [max_threads] [repeats]

set -euo pipefail

SECONDS_PER_RUN=${1:-3}
MAX_THREADS=${2:-$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 8)}
REPEATS=${3:-3}
BIN="./build/cdsbench"

ARCH="${LB_ARCH:-$(uname -m)}"
OUT_DIR="${LB_RESULTS:-results/$ARCH}/cdsbench"
CSV="$OUT_DIR/cdsbench.csv"

PIN_FLAG=""
[ "$(uname)" = "Linux" ] && PIN_FLAG="--pin"

if [ ! -x "$BIN" ]; then
  echo "Build first: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target cdsbench"
  exit 1
fi

mkdir -p "$OUT_DIR"
rm -f "$CSV"

# Power-of-2 thread ladder up to MAX_THREADS, plus the cap if not pow-of-2.
THREAD_LIST=$(awk -v n="$MAX_THREADS" 'BEGIN { t=1; while (t<=n) { printf "%d ", t; t*=2 } if ((t/2) != n) printf "%d", n }')

# StripedMap stripe-lock primitives wired in cds_bench.cpp.
LOCKS="std tas ttas cas ticket"

echo "=== Uniform, 80% read / 10% insert / 10% delete [repeats=$REPEATS] ==="
for lock in $LOCKS; do
  for t in $THREAD_LIST; do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    for r in $(seq 1 "$REPEATS"); do
      $BIN --lock "$lock" --dist uniform --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 3 --read_pct 80 --insert_pct 10 $PIN_FLAG --csv "$CSV"
    done
  done
done
echo ""

echo "=== Zipfian (theta=0.99), 80/10/10 ==="
for lock in $LOCKS; do
  for t in $THREAD_LIST; do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    for r in $(seq 1 "$REPEATS"); do
      $BIN --lock "$lock" --dist zipfian --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 3 --read_pct 80 --insert_pct 10 $PIN_FLAG --csv "$CSV"
    done
  done
done
echo ""

echo "=== Read-heavy: 90% read / 5% insert / 5% delete, Uniform ==="
for lock in $LOCKS; do
  for t in $THREAD_LIST; do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    for r in $(seq 1 "$REPEATS"); do
      $BIN --lock "$lock" --dist uniform --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 3 --read_pct 90 --insert_pct 5 $PIN_FLAG --csv "$CSV"
    done
  done
done
echo ""

echo "=== Write-heavy: 20% read / 40% insert / 40% delete, Zipfian ==="
for lock in $LOCKS; do
  for t in $THREAD_LIST; do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    for r in $(seq 1 "$REPEATS"); do
      $BIN --lock "$lock" --dist zipfian --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup 3 --read_pct 20 --insert_pct 40 $PIN_FLAG --csv "$CSV"
    done
  done
done
echo ""

echo "=== Contention sweep: varying initial bucket count (4 threads, uniform, 80/10/10) ==="
# Note: with default load_factor_resizing<4>, small initial bucket counts
# will trigger many resize events during the run. This is intentional —
# it surfaces the cost of the global-lock resize protocol per primitive.
for lock in ttas cas ticket; do
  for buckets in 256 1024 4096 16384 65536 262144; do
    for r in $(seq 1 "$REPEATS"); do
      $BIN --lock "$lock" --dist uniform --threads 4 --seconds "$SECONDS_PER_RUN" --warmup 3 --read_pct 80 --insert_pct 10 --buckets "$buckets" $PIN_FLAG --csv "$CSV"
    done
  done
done
echo ""

echo "=== Resize-stress: 30% read / 65% insert / 5% delete, --buckets 256 --prefill 0 ==="
# Designed to expose how each lock primitive behaves under repeated
# global resize events. Starting with 256 buckets and an imbalanced
# write mix (65% inserts vs 5% deletes), the StripedMap's
# load_factor_resizing<4> threshold fires many times during the run.
# Each resize takes ALL stripe locks via scoped_full_lock — spinlocks
# (tas/ttas/cas/ticket) burn cycles waiting at this barrier, while
# std::mutex parks via futex/os_unfair_lock. Compare across locks
# at multiple thread counts to see the dropoff.
for lock in $LOCKS; do
  for t in $THREAD_LIST; do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    for r in $(seq 1 "$REPEATS"); do
      $BIN --lock "$lock" --dist uniform --threads "$t" \
        --seconds "$SECONDS_PER_RUN" --warmup 1 \
        --read_pct 30 --insert_pct 65 \
        --buckets 256 --prefill 0 --key_range 5000000 \
        $PIN_FLAG --csv "$CSV"
    done
  done
done
