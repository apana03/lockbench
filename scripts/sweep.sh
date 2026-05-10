#!/usr/bin/env bash
# sweep.sh — Run lockbench across thread counts and lock types.
# Usage: ./scripts/sweep.sh [seconds] [max_threads] [repeats] [cs_lock]
#
#   cs_lock: if provided, run the critical section cost sweep with that lock
#            (tas|ttas|cas|ticket|rw|occ). Omit or pass '-' to skip.
#
# Per-arch defaults: x86 (Xeon, no sudo) gets longer trials, fewer repeats —
# steady-state amortizes scheduling jitter into a single trial. Override via $1/$3.

set -euo pipefail

ARCH="${LB_ARCH:-$(uname -m)}"
case "$ARCH" in
  x86_64)         DEFAULT_SECONDS=20; DEFAULT_WARMUP=4; DEFAULT_REPEATS=3 ;;
  aarch64|arm64)  DEFAULT_SECONDS=10; DEFAULT_WARMUP=2; DEFAULT_REPEATS=3 ;;
  *)              DEFAULT_SECONDS=10; DEFAULT_WARMUP=2; DEFAULT_REPEATS=3 ;;
esac

SECONDS_PER_RUN=${1:-$DEFAULT_SECONDS}
MAX_THREADS=${2:-$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 8)}
REPEATS=${3:-$DEFAULT_REPEATS}
CS_LOCK=${4:-}
WARMUP=${LB_WARMUP:-$DEFAULT_WARMUP}
BIN="./build/lockbench"
OUT_DIR="results/lockbench"
CSV="$OUT_DIR/lockbench.csv"

# auto-detect platform and choose pinning policy on Linux
# compact_phys = one logical thread per physical core, socket 0 first; eliminates
# SMT-sibling contention without root.
PIN_FLAGS=""
[ "$(uname)" = "Linux" ] && PIN_FLAGS="--pin_policy compact_phys"

if [ ! -x "$BIN" ]; then
  echo "Build first: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
  exit 1
fi

mkdir -p "$OUT_DIR"
rm -f "$CSV"

echo "=== Mutex workload (exclusive lock/unlock) [repeats=$REPEATS] ==="
for lock in tas ttas cas ticket rw occ; do
  for t in 1 2 4 6 8 $(seq 16 16 "$MAX_THREADS" 2>/dev/null); do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    for r in $(seq 1 "$REPEATS"); do
      $BIN --lock "$lock" --workload mutex --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup "$WARMUP" $PIN_FLAGS --csv "$CSV"
    done
  done
  echo ""
done

echo "=== Read-sensitive locks: thread sweep (rw, occ, rcu × 0/50/100% reads) ==="
for lock in rw occ; do
  for read_pct in 0 50 80 95 100; do
    for t in 1 2 4 6 8 $(seq 16 16 "$MAX_THREADS" 2>/dev/null); do
      [ "$t" -gt "$MAX_THREADS" ] && continue
      for r in $(seq 1 "$REPEATS"); do
        $BIN --lock "$lock" --workload rw --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup "$WARMUP" --read_pct "$read_pct" $PIN_FLAGS --csv "$CSV"
      done
    done
  done
  echo ""
done

for read_pct in 0 50 80 95 100; do
  for t in 1 2 4 6 8 $(seq 16 16 "$MAX_THREADS" 2>/dev/null); do
    [ "$t" -gt "$MAX_THREADS" ] && continue
    for r in $(seq 1 "$REPEATS"); do
      $BIN --lock rcu --workload rcu --threads "$t" --seconds "$SECONDS_PER_RUN" --warmup "$WARMUP" --read_pct "$read_pct" $PIN_FLAGS --csv "$CSV"
    done
  done
done
echo ""

if [ -n "$CS_LOCK" ] && [ "$CS_LOCK" != "-" ]; then
  echo "=== Critical section cost sweep (4 threads, lock=$CS_LOCK) ==="
  for work in 0 50 100 500 1000 5000; do
    for r in $(seq 1 "$REPEATS"); do
      $BIN --lock "$CS_LOCK" --workload mutex --threads 4 --seconds "$SECONDS_PER_RUN" --warmup "$WARMUP" --cs_work "$work" $PIN_FLAGS --csv "$CSV"
    done
  done
else
  echo "(skipping CS sweep — pass a lock name as \$4 to enable, e.g. ttas)"
fi
