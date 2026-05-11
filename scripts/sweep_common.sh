#!/usr/bin/env bash
# sweep_common.sh — Shared scaffolding for index/lock sweep scripts.
# Sourced by wh_compare.sh, cds_sweep.sh, run_avl_compare.sh.
#
# Provides:
#   - Per-arch defaults: DEFAULT_SECONDS, DEFAULT_WARMUP, DEFAULT_REPEATS
#   - PIN_FLAGS (compact_phys on Linux, empty elsewhere)
#   - WRAP (caffeinate -dim on macOS)
#   - WORKLOADS array (cache-regime matrix: L1/L3 × cold/warm/hot × 95-2/50-25)
#   - compute_thread_ladder() emitting topology-aware thread counts
#   - run_workload_matrix_on <bin> <csv> [extra_args...] runs the matrix
#
# Conventions: callers set SECONDS_PER_RUN / WARMUP / REPEATS before calling
# run_workload_matrix_on. Defaults come from this file's per-arch block.

# Idempotent guard so multiple sources are safe.
if [ -n "${LB_SWEEP_COMMON_LOADED:-}" ]; then return 0 2>/dev/null || true; fi
LB_SWEEP_COMMON_LOADED=1

LB_ARCH="${LB_ARCH:-$(uname -m)}"
# Same per-trial budget on every arch so wall-clock per data point is comparable
# across machines. 5 s × 3 repeats with a 2 s warmup is enough post-warmup window
# to amortize scheduling jitter once compact_phys pinning is in effect; see D23
# in docs/INDEX_LOCK_DECISIONS.md for the budget rationale.
DEFAULT_SECONDS=5
DEFAULT_WARMUP=2
DEFAULT_REPEATS=3

# Linux: SMT-aware pinning (compact_phys). No sudo required.
PIN_FLAGS=""
[ "$(uname)" = "Linux" ] && PIN_FLAGS="--pin_policy compact_phys"

# macOS: caffeinate keeps the system from idle-sleeping mid-sweep.
WRAP=""
if [ "$(uname)" = "Darwin" ] && command -v caffeinate >/dev/null 2>&1; then
  WRAP="caffeinate -dim"
fi

# Cache-regime workload matrix. Format: "rd_pct ins_pct dist label [extra_args...]"
# extra_args is everything after the label (e.g. --key_range, --zipf_theta).
#
# Two cache regimes:
#   L1 — index fits in L1 of every core (key_range=1k, prefill=500). Isolates lock cost.
#   L3 — per-core L1/L2 miss; L3 hit on Xeon (key_range=100k, prefill=50k).
#
# Three skew levels per regime (for read-heavy):
#   warm     zipf θ=0.99   standard YCSB hot-key concentration (~80% ops on ~20% keys)
#   hot      zipf θ=1.2    aggressive concentration; top few keys get most ops
#   extreme  zipf θ=1.5    near-pathological skew; bulk of ops on a handful of keys
# Plus a cold-uniform L3 baseline so we can see what locks do without skew at all.
#
# Two read-heavy mixes:
#   90/5/5   read-heavy YCSB-B style (was 95/2/3 — replaced for more even writer cost)
#   50/25/25 write-heavy
#
# Distributions per regime: 1 uniform baseline + 3 zipfian skews (warm/hot/extreme).
# Read-heavy (8) + write-heavy (4) = 12 workloads. Uniform at every (cache × mix)
# cell gives a no-skew baseline against which the zipfian variants reveal the
# marginal effect of key concentration on lock contention.
WORKLOADS=(
  # --- Read-heavy 90/5/5 across cache × distribution ---
  "90 5  uniform L1_cold_1k               --key_range 1000   --prefill 500"
  "90 5  zipfian L1_warm_zipf99           --key_range 1000   --zipf_theta 0.99 --prefill 500"
  "90 5  zipfian L1_hot_zipf12            --key_range 1000   --zipf_theta 1.2  --prefill 500"
  "90 5  zipfian L1_extreme_zipf15        --key_range 1000   --zipf_theta 1.5  --prefill 500"
  "90 5  uniform L3_cold_100k             --key_range 100000 --prefill 50000"
  "90 5  zipfian L3_warm_100k_zipf99      --key_range 100000 --zipf_theta 0.99 --prefill 50000"
  "90 5  zipfian L3_hot_100k_zipf12       --key_range 100000 --zipf_theta 1.2  --prefill 50000"
  "90 5  zipfian L3_extreme_100k_zipf15   --key_range 100000 --zipf_theta 1.5  --prefill 50000"
  # --- Write-heavy 50/25/25 across cache × distribution ---
  "50 25 uniform L1_50r_uniform           --key_range 1000   --prefill 500"
  "50 25 zipfian L1_50r_zipf99            --key_range 1000   --zipf_theta 0.99 --prefill 500"
  "50 25 uniform L3_50r_uniform           --key_range 100000 --prefill 50000"
  "50 25 zipfian L3_50r_zipf99            --key_range 100000 --zipf_theta 0.99 --prefill 50000"
)

# Topology-aware thread ladder, capped at single-socket physical cores
# (see D23 in docs/INDEX_LOCK_DECISIONS.md). Phases:
#   1. Powers of 2 within socket 0's physical-core count (single-socket fill).
#   2. Append socket 0's physical-core count itself (single-socket saturated).
# Cross-socket (NUMA) and full-machine (SMT) phases are intentionally OMITTED
# so multi-socket Xeon runs only exercise socket 0 — keeps lock-vs-lock
# comparisons clean of cross-socket coherence cost. To re-enable, restore the
# two `[ ... ] && ladder="..."` lines at the end of this function.
# Falls back to power-of-2 up to nproc when sysfs is unavailable.
#
# E.g.: Xeon E5-2650L v3 (12 phys × 2 sock × 2 SMT) → "1 2 4 8 12"
#       Apple M3 (no sysfs)                          → "1 2 4 8 11"
#       Graviton2 (64 phys × 1 sock × 1 SMT)         → "1 2 4 8 16 32 64"
compute_thread_ladder() {
  local sysfs="/sys/devices/system/cpu"
  if [ ! -d "$sysfs/cpu0/topology" ]; then
    local n
    n=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 8)
    awk -v n="$n" 'BEGIN { t=1; while (t<=n) { printf "%d ", t; t*=2 } if ((t/2) != n) printf "%d", n }'
    return
  fi
  local pairs
  pairs=$(for d in "$sysfs"/cpu[0-9]*/topology; do
    [ -d "$d" ] || continue
    local pkg core
    pkg=$(cat "$d/physical_package_id" 2>/dev/null) || continue
    core=$(cat "$d/core_id" 2>/dev/null) || continue
    printf '%s\t%s\n' "$pkg" "$core"
  done | sort -u)
  local n_logical total_phys n_sockets phys_socket0
  n_logical=$(ls -d "$sysfs"/cpu[0-9]*/topology 2>/dev/null | wc -l | tr -d ' ')
  total_phys=$(printf '%s\n' "$pairs" | wc -l | tr -d ' ')
  n_sockets=$(printf '%s\n' "$pairs" | awk '{print $1}' | sort -u | wc -l | tr -d ' ')
  phys_socket0=$(printf '%s\n' "$pairs" | awk '$1==0' | wc -l | tr -d ' ')
  [ "$phys_socket0" -lt 1 ] && phys_socket0="$total_phys"

  local ladder="" t=1
  while [ "$t" -lt "$phys_socket0" ]; do
    ladder="$ladder $t"
    t=$((t * 2))
  done
  ladder="$ladder $phys_socket0"
  # D23: single-socket cap on multi-socket boxes. Cross-socket and SMT phases
  # are intentionally omitted; uncomment the next two lines to restore them.
  # [ "$n_sockets" -gt 1 ]  && ladder="$ladder $total_phys"
  # [ "$n_logical" -gt "$total_phys" ] && ladder="$ladder $n_logical"
  echo "$ladder" | xargs
}

# Run the cache-regime workload matrix against $bin, writing to $csv.
# Caller must set SECONDS_PER_RUN, WARMUP, REPEATS. Extra args after $csv
# are passed to every binary invocation (e.g. --lock <name> for benches
# whose lock is selected by flag rather than by binary name).
#
# Usage:
#   For wormhole (lock baked in to binary name):
#     run_workload_matrix_on ./build/wh_bench_tas "$WH_CSV"
#   For cds/avl (lock selected by --lock flag):
#     run_workload_matrix_on ./build/cdsbench "$CDS_CSV" --lock tas
run_workload_matrix_on() {
  local bin="$1" csv="$2"
  shift 2
  local extra_fixed=("$@")
  local threads
  threads=$(compute_thread_ladder)
  if [ ! -x "$bin" ]; then
    echo "missing: $bin (build first: cmake --build build)" >&2
    return 1
  fi
  local w rd ins dist lbl ext
  for w in "${WORKLOADS[@]}"; do
    read -r rd ins dist lbl ext <<<"$w"
    local t r
    for t in $threads; do
      for r in $(seq 1 "$REPEATS"); do
        # shellcheck disable=SC2086
        $WRAP "$bin" --dist "$dist" --threads "$t" \
          --seconds "$SECONDS_PER_RUN" --warmup "$WARMUP" \
          --read_pct "$rd" --insert_pct "$ins" \
          $PIN_FLAGS $ext "${extra_fixed[@]}" \
          --csv "$csv" >/dev/null
      done
    done
    echo "[$(basename "$bin")] $lbl ($dist $rd/$ins/$((100-rd-ins))) ladder='$threads' repeats=$REPEATS"
  done
}
