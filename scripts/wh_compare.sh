#!/usr/bin/env bash
# wh_compare.sh — Sweep wormhole across lock variants on the shared
# cache-regime workload matrix.
#
# Lock list (wormhole-specific; selected at compile time per binary):
#   default      — wormhole's stock rwlock (counter-based)
#   tas/ttas/cas — exclusive spinlocks (used as both rwlock paths in the shim)
#   occ          — optimistic-read seqlock (write-side only via shim)
#   occ-opt      — optimistic readers walk leaves without leaflock (lock-free reads)
#   pcpu-rw      — per-CPU rwlock (reader-scalable; new — see D9 in INDEX_LOCK_DECISIONS.md)
#
# All other knobs (workloads, threads, pinning, repeats) come from
# scripts/sweep_common.sh so this matches cds_sweep.sh and run_avl_compare.sh.
#
# Args: ./wh_compare.sh [seconds] [warmup] [repeats]
#       ./wh_compare.sh --quick      (5s × 1 repeat for iteration)

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

OUT_DIR="${LB_RESULTS:-results/$LB_ARCH}/wh_compare"
WH_CSV="$OUT_DIR/wh.csv"
mkdir -p "$OUT_DIR"
rm -f "$WH_CSV"

LOCKS="default tas ttas cas occ occ-opt pcpu-rw"

echo "=== wormhole sweep (arch=$LB_ARCH seconds=$SECONDS_PER_RUN warmup=$WARMUP repeats=$REPEATS) ==="
for lock in $LOCKS; do
  run_workload_matrix_on "./build/wh_bench_${lock}" "$WH_CSV"
done
echo "Done. CSV in $WH_CSV"
