#!/usr/bin/env bash
# cds_sweep.sh — Sweep cdsbench (libcds StripedMap) across lock variants on
# the shared cache-regime workload matrix, plus StripedMap-specific
# specialized sections (bucket-count sweep, resize-stress).
#
# Lock list: stripe-lock primitives wired in cds_bench.cpp (no rwlock variants —
# StripedMap embeds an exclusive lock per stripe).
#
# Args: ./cds_sweep.sh [seconds] [warmup] [repeats]
#       ./cds_sweep.sh --quick      (5s × 1 repeat for iteration)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/sweep_common.sh"

if [ "${1:-}" = "--quick" ]; then
  SECONDS_PER_RUN=5; WARMUP=1; REPEATS=1
  shift
else
  SECONDS_PER_RUN=${1:-$DEFAULT_SECONDS}
  WARMUP=${2:-$DEFAULT_WARMUP}
  REPEATS=${3:-$DEFAULT_REPEATS}
fi

BIN="./build/cdsbench"
OUT_DIR="${LB_RESULTS:-results/$LB_ARCH}/cdsbench"
CSV="$OUT_DIR/cdsbench.csv"

if [ ! -x "$BIN" ]; then
  echo "Build first: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target cdsbench"
  exit 1
fi

mkdir -p "$OUT_DIR"
rm -f "$CSV"

LOCKS="std tas ttas cas ticket"

# === Shared cache-regime matrix ===
echo "=== StripedMap sweep (arch=$LB_ARCH seconds=$SECONDS_PER_RUN warmup=$WARMUP repeats=$REPEATS) ==="
for lock in $LOCKS; do
  run_workload_matrix_on "$BIN" "$CSV" --lock "$lock"
done
echo ""

# === StripedMap-specific: bucket-count sweep at fixed 4 threads ===
# Probes the global-lock resize protocol cost as load factor crosses
# load_factor_resizing<4>'s threshold during the run.
THREADS_BUCKET=4
echo "=== Bucket-count sweep (4 threads, uniform 80/10/10) ==="
for lock in ttas cas ticket; do
  for buckets in 256 1024 4096 16384 65536 262144; do
    for r in $(seq 1 "$REPEATS"); do
      # shellcheck disable=SC2086
      $WRAP "$BIN" --lock "$lock" --dist uniform --threads "$THREADS_BUCKET" \
        --seconds "$SECONDS_PER_RUN" --warmup "$WARMUP" \
        --read_pct 80 --insert_pct 10 --buckets "$buckets" \
        $PIN_FLAGS --csv "$CSV" >/dev/null
    done
  done
done
echo ""

# === Resize-stress: small initial buckets + write-heavy mix ===
# Designed to fire repeated global resize events. Each resize takes ALL
# stripe locks via scoped_full_lock — spinlocks burn cycles waiting at
# this barrier; std::mutex parks via futex/os_unfair_lock.
echo "=== Resize-stress (30/65/5 uniform, --buckets 256 --prefill 0 --key_range 5M) ==="
THREADS=$(compute_thread_ladder)
for lock in $LOCKS; do
  for t in $THREADS; do
    for r in $(seq 1 "$REPEATS"); do
      # shellcheck disable=SC2086
      $WRAP "$BIN" --lock "$lock" --dist uniform --threads "$t" \
        --seconds "$SECONDS_PER_RUN" --warmup "$WARMUP" \
        --read_pct 30 --insert_pct 65 \
        --buckets 256 --prefill 0 --key_range 5000000 \
        $PIN_FLAGS --csv "$CSV" >/dev/null
    done
  done
done
echo "Done. CSV in $CSV"
