#!/usr/bin/env bash
# run_avl_compare.sh — Sweep cdsbench (StripedMap) and cds_avl_bench (BronsonAVL)
# across the same workloads/locks/threads. Designed for the comparison notebook.
#
# macOS "CPU hints":
#   - caffeinate: prevent App Nap / sleep during the run
#   - nice -n -10: slight scheduler priority boost
#   - taskpolicy is intentionally NOT used; default policy lets schedulers place
#     threads on Performance cores when active.
#
# Args: ./run_avl_compare.sh [seconds] [warmup]

set -euo pipefail

SECONDS_PER_RUN=${1:-3}
WARMUP=${2:-1}

ARCH="${LB_ARCH:-$(uname -m)}"
OUT_DIR="${LB_RESULTS:-results/$ARCH}/avl_compare"
CDS_CSV="$OUT_DIR/cds_striped.csv"
AVL_CSV="$OUT_DIR/cds_avl.csv"

mkdir -p "$OUT_DIR"
rm -f "$CDS_CSV" "$AVL_CSV"

if [ "$(uname)" = "Darwin" ] && command -v caffeinate >/dev/null 2>&1; then
  # -i prevent idle sleep, -d prevent display sleep, -m disk
  WRAP="caffeinate -dim"
else
  WRAP=""
fi

# Power-of-2 thread ladder up to logical CPU count, plus the cap if not pow-of-2.
NCPU=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 8)
THREADS=$(awk -v n="$NCPU" 'BEGIN { t=1; while (t<=n) { printf "%d ", t; t*=2 } if ((t/2) != n) printf "%d", n }')
LOCKS="std tas ttas cas ticket"
WORKLOADS=(
  "80 10 uniform balanced"
  "80 10 zipfian zipf"
  "90 5  uniform read_heavy"
  "20 40 zipfian write_heavy"
)

run_one() {
  local bin="$1" csv="$2"
  for w in "${WORKLOADS[@]}"; do
    set -- $w; rd=$1; ins=$2; dist=$3; lbl=$4
    for lock in $LOCKS; do
      for t in $THREADS; do
        $WRAP "$bin" \
          --lock "$lock" --dist "$dist" --threads "$t" \
          --seconds "$SECONDS_PER_RUN" --warmup "$WARMUP" \
          --read_pct "$rd" --insert_pct "$ins" \
          --csv "$csv" >/dev/null
      done
    done
    echo "[$bin] $lbl ($dist $rd/$ins/$((100-rd-ins)))"
  done
}

echo "=== cdsbench (StripedMap) seconds=$SECONDS_PER_RUN warmup=$WARMUP ==="
run_one ./build/cdsbench       "$CDS_CSV"
echo "=== cds_avl_bench (BronsonAVL) seconds=$SECONDS_PER_RUN warmup=$WARMUP ==="
run_one ./build/cds_avl_bench  "$AVL_CSV"
echo "Done. CSVs in $OUT_DIR"
