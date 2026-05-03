#!/usr/bin/env bash
# wh_compare.sh — Sweep wormhole across lock variants, workloads, and threads.
# 6 binaries × 4 workloads × 4 thread points × 1 repeat. Ticket lock is
# excluded by design (see CMakeLists.txt + EXPERIMENT.md).
#
# Args: ./wh_compare.sh [seconds] [warmup]

set -euo pipefail

SECONDS_PER_RUN=${1:-3}
WARMUP=${2:-1}

ARCH="${LB_ARCH:-$(uname -m)}"
OUT_DIR="${LB_RESULTS:-results/$ARCH}/wh_compare"
WH_CSV="$OUT_DIR/wh.csv"

mkdir -p "$OUT_DIR"
rm -f "$WH_CSV"

# macOS: caffeinate keeps the system from idle-sleeping mid-sweep.
WRAP=""
if [ "$(uname)" = "Darwin" ] && command -v caffeinate >/dev/null 2>&1; then
  WRAP="caffeinate -dim"
fi

# Power-of-2 thread ladder up to logical CPU count, plus the cap if
# it isn't already a power of 2.
NCPU=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 8)
THREADS=$(awk -v n="$NCPU" 'BEGIN { t=1; while (t<=n) { printf "%d ", t; t*=2 } if ((t/2) != n) printf "%d", n }')

LOCKS="default rw tas ttas cas occ occ-opt"
WORKLOADS=(
  "80 10 uniform balanced"
  "80 10 zipfian zipf"
  "90 5  uniform read_heavy"
  "20 40 zipfian write_heavy"
)

echo "=== wormhole sweep (seconds=$SECONDS_PER_RUN warmup=$WARMUP) ==="
for w in "${WORKLOADS[@]}"; do
  set -- $w; rd=$1; ins=$2; dist=$3; lbl=$4
  for lock in $LOCKS; do
    bin="./build/wh_bench_${lock}"
    if [ ! -x "$bin" ]; then
      echo "missing: $bin (build first: cmake --build build)"
      exit 1
    fi
    for t in $THREADS; do
      $WRAP "$bin" --dist "$dist" --threads "$t" \
        --seconds "$SECONDS_PER_RUN" --warmup "$WARMUP" \
        --read_pct "$rd" --insert_pct "$ins" \
        --csv "$WH_CSV" >/dev/null
    done
  done
  echo "[wormhole] $lbl ($dist $rd/$ins/$((100-rd-ins)))"
done
echo "Done. CSV in $WH_CSV"
