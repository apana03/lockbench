#!/usr/bin/env bash
# run_avl_compare.sh — Sweep cdsbench (StripedMap) and cds_avl_bench
# (BronsonAVL) side-by-side on the shared cache-regime workload matrix.
#
# Same workloads, same locks (per-bench), same threads, same hygiene as
# wh_compare.sh and cds_sweep.sh. Designed for the comparison notebook.
#
# Args: ./run_avl_compare.sh [seconds] [warmup] [repeats]
#       ./run_avl_compare.sh --quick      (5s × 1 repeat for iteration)

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

OUT_DIR="${LB_RESULTS:-results/$LB_ARCH}/avl_compare"
CDS_CSV="$OUT_DIR/cds_striped.csv"
AVL_CSV="$OUT_DIR/cds_avl.csv"

mkdir -p "$OUT_DIR"
rm -f "$CDS_CSV" "$AVL_CSV"

LOCKS="std tas ttas cas ticket"

echo "=== cdsbench (StripedMap) (arch=$LB_ARCH seconds=$SECONDS_PER_RUN warmup=$WARMUP repeats=$REPEATS) ==="
for lock in $LOCKS; do
  run_workload_matrix_on ./build/cdsbench "$CDS_CSV" --lock "$lock"
done

echo "=== cds_avl_bench (BronsonAVL) ==="
for lock in $LOCKS; do
  run_workload_matrix_on ./build/cds_avl_bench "$AVL_CSV" --lock "$lock"
done
echo "Done. CSVs in $OUT_DIR"
