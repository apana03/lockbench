#!/usr/bin/env bash
# run_all_sweeps.sh — One-command per-machine driver.
#
# Runs all three index sweeps (cdsbench, cds_avl_bench, wh_bench) for the
# current architecture, writing results to results/$LB_ARCH/. Defaults to
# uname -m for LB_ARCH; override with LB_ARCH=graviton-c7g (or similar)
# to keep instance-type detail for cross-arch analysis.
#
# Args: ./run_all_sweeps.sh [seconds] [warmup]
#   seconds  — measurement duration per run (default 3)
#   warmup   — warmup duration per run (default 1)

set -euo pipefail

SECONDS_PER_RUN=${1:-3}
WARMUP=${2:-1}

ARCH="${LB_ARCH:-$(uname -m)}"
export LB_ARCH="$ARCH"

# Resolve project root from script location so the script works from
# anywhere.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

if [ ! -x ./build/wh_bench_default ]; then
  echo "ERROR: build artifacts missing. Run:"
  echo "  cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DWH_FAIR_MM=ON"
  echo "  cmake --build build -j"
  exit 1
fi

echo "=== Cross-arch sweep on LB_ARCH=$ARCH ==="
echo "    Hostname: $(hostname -s 2>/dev/null || hostname)"
echo "    Cores:    $(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo '?')"
echo "    Output:   results/$ARCH/{cdsbench,avl_compare,wh_compare}/"
echo ""

# StripedMap (uses the cds_sweep.sh contention block by default; pass
# repeats=1 to match the other two sweeps).
echo "[1/3] cdsbench (libcds StripedMap)"
./scripts/cds_sweep.sh "$SECONDS_PER_RUN" "" 1
echo ""

# StripedMap + BronsonAVL combined (same harness pattern).
echo "[2/3] cds_avl_bench (libcds Bronson AVL) + cdsbench (paired)"
./scripts/run_avl_compare.sh "$SECONDS_PER_RUN" "$WARMUP"
echo ""

# Wormhole — 7 variants.
echo "[3/3] wh_bench (Wormhole, 7 lock variants)"
./scripts/wh_compare.sh "$SECONDS_PER_RUN" "$WARMUP"
echo ""

echo "=== Done. Results: results/$ARCH/ ==="
ls -la "results/$ARCH/" 2>/dev/null || true
