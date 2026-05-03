#!/usr/bin/env bash
# quick_compare.sh — cdsbench sweep for comparison plots.
# 4 workload mixes × 5 stripe-lock primitives × threads {1,2,4,8}.
# Defaults: --seconds 8 --warmup 3, no repeats. ~14 min on M-series.
# Override with: ./quick_compare.sh [seconds] [warmup]

set -euo pipefail

SECONDS_PER_RUN=${1:-8}
WARMUP=${2:-3}

OUT_DIR="results/quick_compare"
CDS_CSV="$OUT_DIR/cds.csv"

PIN_FLAG=""
[ "$(uname)" = "Linux" ] && PIN_FLAG="--pin"

mkdir -p "$OUT_DIR"
rm -f "$CDS_CSV"

THREADS="1 2 4 8"

# read_pct insert_pct dist label
WORKLOADS=(
  "80 10 uniform balanced"
  "80 10 zipfian zipf"
  "90 5  uniform read_heavy"
  "20 40 zipfian write_heavy"
)

CDS_LOCKS="std tas ttas cas ticket"

echo "=== cdsbench (seconds=$SECONDS_PER_RUN warmup=$WARMUP) ==="
for w in "${WORKLOADS[@]}"; do
  set -- $w; rd=$1; ins=$2; dist=$3; lbl=$4
  for lock in $CDS_LOCKS; do
    for t in $THREADS; do
      ./build/cdsbench --lock "$lock" --dist "$dist" --threads "$t" \
           --seconds "$SECONDS_PER_RUN" --warmup "$WARMUP" \
           --read_pct "$rd" --insert_pct "$ins" \
           $PIN_FLAG --csv "$CDS_CSV" >/dev/null
    done
  done
  echo "[cdsbench] done $lbl ($dist $rd/$ins/$((100-rd-ins)))"
done
echo "Done. CSV in $CDS_CSV"
