# Index Lock Benchmark — Decisions Log

This file tracks methodology decisions for the index-level lock benchmarks (lockbench microbench + wormhole sweep). Append further decisions as the work progresses; the format is `YYYY-MM-DD — short title`, then problem statement, diagnostic findings, and a decisions table.

## 2026-05-10 — Initial diagnosis and overhaul

### Problem statement

1. Xeon variance ~10–30% CoV — too high to publish.
2. Wormhole asymmetry: spinlock-as-mutex variants (`wh-tas`, `wh-ttas`, `wh-cas`) match/beat the default rwlock; `wh-rw` (custom counter-based rwlock) does not beat spinlocks on read-heavy workloads. Suspicious.
3. Lock-to-lock differences ~5%, below harness noise floor.
4. Suspect keys fit in cache at the current `prefill=500k` setting, leaving readers almost never co-accessing the same wormhole leaf.

### Diagnostic findings

- `rw_lock` (`include/primitives/rw_lock.hpp:9-58`) is semantically reader-parallel but cache-coherence-serialized: every `read_lock` does a CAS on a single shared `state` counter; every `read_unlock` does a `fetch_sub` on the same line. The cache line ping-pongs between cores on every operation. For sub-100 ns critical sections (wormhole leaf lookups), the CAS round-trip dominates and the reader-parallelism doesn't materialize as throughput. Same limitation as `std::shared_mutex`. Reference: Calciu et al. 2013, "NUMA-Aware Reader-Writer Locks."
- Wormhole's stock rwlock (`wh-default`) IS itself a counter-based rwlock with the same property → `wh-rw` is redundant.
- Per-op `measuring.load()` and `stop.load()` in the timed loop add measurable overhead at 30–80 ns ops; two `mt19937` RNG draws per op (key + op selector) add more.
- `wh_compare.sh` does NOT pass `--pin` (only `sweep.sh` does), so all wormhole sweeps on Linux/Xeon were unpinned — high variance from cross-socket placement and SMT-sibling collisions.
- Current workloads (`key_range=1M`, uniform, 8 threads) sit at the L3 boundary with near-zero reader-reader contention.

### Decisions taken

| #   | Decision                                                                                                                                                                                                                            | Rationale                                                                                                                                                                                                |
| --- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| D1  | Add SMT-aware pinning (`pin_policy=compact_phys` default on Linux)                                                                                                                                                                  | One logical thread per physical core, socket 0 first. Eliminates SMT-sibling contention without disabling SMT (no sudo needed). Single biggest variance reducer.                                         |
| D2  | Pin threads on Xeon without sudo                                                                                                                                                                                                    | `sched_setaffinity` against own process is unprivileged. Plumbing exists in `setup_worker_thread`; the missing piece is `wh_compare.sh` not passing the flag.                                            |
| D3  | Two-phase timed loop (warmup → barrier → measurement)                                                                                                                                                                               | Removes per-op `measuring.load()` and the conditional branch on it. Hot loop becomes lock+CS+unlock+`++local`.                                                                                           |
| D4  | Strided `stop` check (every K=64 iters)                                                                                                                                                                                             | Removes per-op `stop.load()`. K=64 → ~6 µs check interval, ~1.6 µs average overshoot at 20M ops/s — negligible vs 20s window.                                                                            |
| D5  | Python aggregator with median + IQR + CoV (new `scripts/aggregate.py`)                                                                                                                                                              | Per-trial CSV stays raw; aggregation in Python keeps statistical work out of C++ and preserves reproducibility. CoV >10% triggers a warning.                                                             |
| D6  | Xeon: 20s × 3 repeats; ARM: 10s × 3 repeats                                                                                                                                                                                         | Longer windows capture more steady-state and amortize OS jitter into a single trial; user explicitly preferred fewer-but-longer over more-but-shorter.                                                   |
| D7  | Drop DRAM-bound tier from cache-regime matrix (L1 + L3 only)                                                                                                                                                                        | Memory-latency-dominated regimes compress lock differences toward zero, weakening the lock-comparison story. Lock-bound regimes are what the thesis needs.                                               |
| D8  | Drop `wh-rw` from wormhole comparison                                                                                                                                                                                               | Wormhole's `default` already exemplifies a counter-based rwlock; `wh-rw` is the same design with a different implementation. The `rw_lock` primitive stays in the lockbench microbenchmark.              |
| D9  | Add per-CPU rwlock primitive (`pcpu_rw_lock`, new `wh-pcpu-rw` column)                                                                                                                                                              | Shows what a reader-scalable rwlock can do — each thread has its own counter on its own cache line. Necessary contrast point to the counter-based rwlocks.                                               |
| D10 | Document the cache-coherence story (`docs/EXPERIMENT.md`)                                                                                                                                                                           | Frame the asymmetry as a known limitation of counter-based rwlocks for short CSes, with the per-CPU rwlock as the fix. Cite Calciu et al. 2013.                                                          |
| D11 | Cache-regime workload matrix (L1: key_range=1k; L3: key_range=100k) × (cold uniform / warm zipf 0.99 / hot zipf 1.2) × (95/2 read-heavy + 50/25 write-heavy)                                                                         | Force the lock to be the bottleneck (small footprint = no memory pressure; concentrated keys = reader-reader collisions).                                                                                |
| D12 | No DRAM tier → no parallel prefill needed                                                                                                                                                                                           | Multi-threaded `prefill_index` deferred to Tier 3; can be added if DRAM tier is ever revived.                                                                                                            |
| D13 | Verify shim inlining via `gen_asm.sh` + add LTO if needed                                                                                                                                                                           | The `extern "C" rwlock_lock_read` is a real call boundary from wormhole's C source; without LTO every read op pays a `call`. Re-sweep after enabling.                                                    |
| D14 | Pre-generated (key, op) streams per worker (Tier 3 — nice-to-have)                                                                                                                                                                  | Removes `mt19937` + `zipfian_generator::next_scrambled()` from hot path. 256K-entry circular buffer per thread; statistical properties preserved across replays.                                          |
| D15 | Sampled HDR latency histogram (Tier 3 — `--latency` flag)                                                                                                                                                                           | Adds p50/p99/p99.9 reporting at ~2% perturbation. Useful for "ticket lock has lower p99" discussion.                                                                                                     |

### Open questions / pending

- Whether `wh-default`'s underlying rwlock is exactly counter-based or has a different design — to be confirmed by reading wormhole's `wh.c` lock implementation.
- Whether `pcpu_rw_lock`'s 4 KiB-per-instance cost (64 slots × 64 B) is acceptable for wormhole's per-leaf locks (~3000 leaves × 4 KiB = ~12 MiB at L3-resident scale). If not, slot count is reducible to 32 (still > thread count for our sweeps).

## 2026-05-10 — Post-implementation findings (Apple M3 quick sweep)

After implementing the plan and running `wh_compare.sh --quick` on Apple M3 (4 P-cores + 4 E-cores), three patterns emerged that the user should be aware of before re-running on Xeon.

### Pattern 1 — at 4 threads on a lock-bound regime, `wh-pcpu-rw` wins

| Workload (key_range × zipf, 95% reads) | wh-default | wh-tas | wh-pcpu-rw | wh-occ-opt |
| ---: | ---: | ---: | ---: | ---: |
| L1_warm_zipf99, 4T  | 38.3 M | 37.4 M | **44.3 M** | 47.6 M |
| L1_hot_zipf12, 4T   | 37.5 M | 35.4 M | **45.6 M** | 49.1 M |

This is the central thesis result: at 4T (one thread per P-core), the per-CPU rwlock beats both the counter-based `default` rwlock (no contended cache line) and the spinlock-as-mutex variants (parallel readers actually overlap).

### Pattern 2 — at 8+ threads on M3, `wh-pcpu-rw` collapses (heterogeneous-core artifact)

At 8T on the same L1_warm_zipf99 workload:

| Lock | M3 ops/s | Notes |
| ---: | ---: | --- |
| wh-default  | 25.4 M | ~33 % drop from 4T |
| wh-tas      | 10.8 M | spinlock contention bites |
| wh-pcpu-rw  |  4.65 M | **collapses**; writer drain stalls on E-core readers |
| wh-occ-opt  | 65.5 M | scales unhindered (lock-free reads) |

Likely cause: M3 has 4 P-cores; threads 5–8 land on E-cores via macOS QoS scheduling. The per-CPU rwlock writer must wait for all reader slots to drain before proceeding — when half the readers are running on slower E-cores, the writer-induced reader stall dominates. Counter-based `default` doesn't suffer as badly because its writer is a single CAS.

This is Apple Silicon-specific. On homogeneous-core Xeon the pattern should not appear, and `wh-pcpu-rw` should keep scaling.

### Pattern 3 — in the L3 regime, all rwlock variants converge

At L3_warm_zipf99 with 95 % reads:

| thr | default | tas | pcpu-rw | occ-opt |
| ---: | ---: | ---: | ---: | ---: |
| 1   | 16.7 | 18.5 | 17.0 | 18.8 |
| 4   | 59.9 | 59.3 | 56.8 | 68.9 |
| 8   | 67.9 | 65.5 | 66.4 | 85.3 |
| 11  | 77.4 | 75.3 | 62.3 | 94.1 |

Once the index doesn't fit in L1 and per-op memory latency dominates, lock differences compress (~15 % spread) and `occ-opt` wins because lock-free reads avoid serialization entirely. Confirms decision D7 (drop DRAM tier — same compression effect, more pronounced).

### Action items for the next iteration

- **Validate pcpu-rw on Xeon** (homogeneous cores, no E-core artifact). If wh-pcpu-rw scales monotonically there, Pattern 2 is M3-only and we can document it as a known limitation of macOS scheduling.
- **Optional optimization:** track high-water-mark slot index in `pcpu_rw_lock::my_slot()` and have writers scan only up to that. Reduces scan from 64 → ~thread_count slot loads. Doesn't fix Pattern 2 (which is reader-stall, not scan-cost) but cleaner.
- **Reframe thesis:** the story is now "per-CPU rwlocks are fast at moderate thread counts on homogeneous cores; lock-free OCC reads win at high contention." Both findings are publishable.

## 2026-05-10 — Pinning follow-ups

Two follow-up fixes after the initial pass:

### D16 — Pin main thread to socket 0 during `prefill_index()`

**Problem.** `prefill_index()` (`include/util/bench_harness.hpp:278`) runs on the main thread before workers spawn. Without explicit pinning, the kernel chooses the main thread's CPU arbitrarily, so wormhole's index pages first-touch onto whichever socket the main thread happens to be on. For low-thread-count sweep points (workers fit in socket 0), if prefill landed on socket 1 the workers pay cross-socket latency on every operation — a major run-to-run variance source.

**Fix.** When `--pin_policy != off`, `prefill_index()` now temporarily pins the main thread to slot 0 of the chosen policy (under `compact_phys`: socket 0 P-core 0), runs prefill, then calls a new `clear_thread_affinity()` (`include/primitives/util.hpp`) before workers spawn. New helper resets affinity to every online CPU so workers can be placed under their own per-thread pin policy.

Affects all 6 index benches via the shared helper: `wh_bench`, `index_bench`, `skiplist_bench`, `bptree_bench`, `cds_bench`, `cds_avl_bench`. macOS is a no-op (no `sched_setaffinity`).

### D17 — Topology-aware thread ladder in `wh_compare.sh`

**Problem.** The previous `awk`-driven ladder produced powers of 2 up to logical CPU count, e.g. `1 2 4 8 16 32 48` on the Xeon E5-2650L v3 (12 phys × 2 sock × 2 SMT). The `16` breakpoint awkwardly spilled 4 threads onto socket 1 mid-step — neither a clean single-socket measurement nor a clean cross-socket one. Several sweep points fell at uninterpretable mid-topology positions.

**Fix.** New `compute_thread_ladder()` function in `scripts/wh_compare.sh` reads `/sys/devices/system/cpu/cpu*/topology/{physical_package_id, core_id}` and produces a ladder with **three explicit phases**:

1. Powers of 2 within socket 0's physical-core count.
2. The full cross-socket physical-core count (NUMA boundary).
3. The full logical-CPU count (SMT engaged).

Outputs per topology:

| Machine | Layout | Old ladder | New ladder |
| --- | --- | --- | --- |
| Xeon E5-2650L v3 | 12 phys × 2 sock × 2 SMT = 48 logical | 1 2 4 8 16 32 48 | **1 2 4 8 12 24 48** |
| Graviton2 | 64 phys × 1 sock × 1 SMT = 64 logical | 1 2 4 8 16 32 64 | 1 2 4 8 16 32 64 |
| Apple M3 | sysfs unavailable → fallback to power-of-2 | 1 2 4 8 11 | 1 2 4 8 11 |

Each Xeon breakpoint now corresponds to a real architectural transition: 8→12 saturates socket 0, 12→24 crosses sockets, 24→48 engages SMT. Plot interpretation becomes much cleaner — flat regions and inflection points map to topology, not arbitrary doublings.

Falls back to the old power-of-2 ladder when sysfs is unavailable (containers, macOS).

## 2026-05-10 — D18: Align wormhole / StripedMap / BronsonAVL workloads

Until this change the three sweep scripts had drifted apart: wh_compare.sh used the new cache-regime matrix and topology-aware ladder; cds_sweep.sh used the old default-bench-harness config (key_range=1M, prefill=500k, power-of-2 ladder); run_avl_compare.sh ran without pinning, without per-arch defaults, and with REPEATS=1. Result: lock differences were invisible in the AVL/Map data because the workloads weren't lock-bound, and what little signal existed was buried in unpinned-Xeon variance.

**Decision.** Make everything except the lock list identical across the three benches. Same workload matrix (L1+L3 × cold/warm/hot × 95-2/50-25), same topology-aware ladder, same per-arch defaults (Xeon 20s × 3 / ARM 10s × 3), same compact_phys pinning, same repeats, same `--quick` mode for iteration.

**Implementation.** New `scripts/sweep_common.sh` provides:

- Per-arch `DEFAULT_SECONDS` / `DEFAULT_WARMUP` / `DEFAULT_REPEATS`
- `PIN_FLAGS` (compact_phys on Linux, empty elsewhere)
- `WRAP` (caffeinate -dim on macOS)
- `WORKLOADS` (the cache-regime matrix)
- `compute_thread_ladder()` (topology-aware)
- `run_workload_matrix_on <bin> <csv> [extra_args…]` — runs the full matrix on one binary

Each sweep script becomes thin: defines its lock list, sources the helper, calls `run_workload_matrix_on` once per lock.

**Locks per bench (intentionally different — the only axis that varies):**

| Bench | Locks | Why this list |
| --- | --- | --- |
| wormhole | default, tas, ttas, cas, occ, occ-opt, pcpu-rw | Full set; wormhole's leaflock is rwlock-shaped |
| StripedMap | std, tas, ttas, cas, ticket | StripedMap embeds an exclusive lock per stripe — rwlock variants don't fit |
| BronsonAVL | std, tas, ttas, cas, ticket | Same; the AVL nodes use stripe-style mutex per-node |

`pcpu-rw` and `occ`/`occ-opt` are wormhole-only because the StripedMap and BronsonAVL bench harnesses wire the lock as an exclusive primitive. Adding rwlock support to those would be a separate change in `cds_bench.cpp` / `cds_avl_bench.cpp`; flagged as future work.

**StripedMap-specific specials retained.** `cds_sweep.sh` still runs its bucket-count sweep and resize-stress sections after the shared matrix — those probe StripedMap's resize protocol, which is a different story from per-stripe lock contention. They also now use the topology ladder and `compact_phys` pinning.

## 2026-05-10 — D19: Flatten per-trial budget across arches

Earlier (D6) we set Xeon to 20 s × 3 repeats and ARM to 10 s × 3 repeats. **Revised**: all arches use **10 s × 3 repeats with 2 s warmup**, regardless of `uname -m`.

**Why.**

- Wall-clock per data point is now identical across machines, which makes cross-arch comparison plots directly comparable without per-arch normalisation.
- The previous Xeon-specific 20 s window was a hedge against governor jitter that we already eliminated more cleanly via `compact_phys` pinning + main-thread prefill pinning. With those in place, 10 s is enough on Xeon too.
- If a particular Xeon group still shows >10 % CoV after the new run, the aggregator's CoV-warning surfaces it; we can decide per-group whether to extend, rather than padding every cell.
- Total Xeon publish-run wall-clock drops from ~19 h (across all three sweep scripts) to ~9.5 h. Iteration via `--quick` is unchanged.

**Override.** All three scripts still accept positional args (`[seconds] [warmup] [repeats]`) and the `--quick` shortcut. Override env via `LB_ARCH=...` is still respected for the topology probe but no longer affects time defaults.

## 2026-05-10 — D20: Workload list — read-heavy 90/5/5, more skew levels

Updated the workload matrix in `scripts/sweep_common.sh`:

- **Read-heavy mix changed: 95/2/3 → 90/5/5.** YCSB-B style. 95/2/3 minimised writer cost which made the read-heavy regime nearly read-only at the lock-acquire layer; 90/5/5 keeps the workload genuinely read-heavy while exposing real writer-induced reader stalls.
- **Three skew levels per cache regime** (was: warm-only at L3, warm+hot at L1):
  - `warm`    — zipf θ=0.99 (standard YCSB hot-key concentration)
  - `hot`     — zipf θ=1.2 (aggressive concentration)
  - `extreme` — zipf θ=1.5 (near-pathological skew; bulk of ops on a handful of keys; new)
- **L3 gains hot + extreme variants** (previously only warm). Lets us see the same skew progression at both cache regimes.
- **L1 gains an extreme variant** (`L1_extreme_zipf15`).

Final list (9 workloads, 7 read-heavy + 2 write-heavy):

| # | Label | Mix | Cache | Skew |
| --- | --- | --- | --- | --- |
| 1 | L1_warm_zipf99           | 90/5/5  | L1 | warm θ=0.99 |
| 2 | L1_hot_zipf12            | 90/5/5  | L1 | hot θ=1.2 |
| 3 | L1_extreme_zipf15        | 90/5/5  | L1 | extreme θ=1.5 |
| 4 | L3_cold_100k             | 90/5/5  | L3 | uniform |
| 5 | L3_warm_100k_zipf99      | 90/5/5  | L3 | warm θ=0.99 |
| 6 | L3_hot_100k_zipf12       | 90/5/5  | L3 | hot θ=1.2 |
| 7 | L3_extreme_100k_zipf15   | 90/5/5  | L3 | extreme θ=1.5 |
| 8 | L1_50r_zipf99            | 50/25/25 | L1 | warm θ=0.99 |
| 9 | L3_50r_zipf99            | 50/25/25 | L3 | warm θ=0.99 |

Write-heavy stays at warm only — the writer-writer contention story is more about lock fairness than key skew, and adding hot/extreme variants here would 4× the write-heavy sweep cost for marginal additional signal. Easy to extend later if needed.

**Total trials per arch (9 workloads × 7 ladder pts × 3 repeats):**
- wh_compare:       7 locks × 9 × 7 × 3 = **1,323 trials** (~4.4 h @ 12 s/trial)
- cds_sweep:        5 locks × 9 × 7 × 3 = **945 trials** (matrix only) + specials (~3.7 h)
- run_avl_compare:  10 (5 locks × 2 binaries) × 9 × 7 × 3 = **1,890 trials** (~6.3 h)

`--quick` mode (5 s × 1 repeat) ≈ 12× faster.

## 2026-05-10 — D21: Add uniform-distribution workloads

**Decision.** Added 3 uniform-distribution workloads to fill out the matrix at no-skew baseline:

- `L1_cold_1k`        — 90/5/5 uniform, key_range=1k (was missing — L3 had `L3_cold_100k` but L1 had no uniform variant)
- `L1_50r_uniform`    — 50/25/25 uniform, key_range=1k
- `L3_50r_uniform`    — 50/25/25 uniform, key_range=100k

**Why.** Without a uniform baseline at every cache × mix cell, you can't attribute observed lock differences to key skew vs. lock-implementation properties. With both uniform and zipfian at every regime, the zipfian curves now have a no-skew reference: any departure from uniform-at-same-regime is a skew-induced effect.

**Final matrix (12 workloads = 2 mixes × 2 cache regimes × 3 zipf skews + 4 uniform baselines):**

| Cache | Distribution | 90/5/5 (read) | 50/25/25 (write) |
| --- | --- | --- | --- |
| L1 | uniform        | L1_cold_1k                | L1_50r_uniform |
| L1 | zipf θ=0.99    | L1_warm_zipf99            | L1_50r_zipf99 |
| L1 | zipf θ=1.2     | L1_hot_zipf12             | — |
| L1 | zipf θ=1.5     | L1_extreme_zipf15         | — |
| L3 | uniform        | L3_cold_100k              | L3_50r_uniform |
| L3 | zipf θ=0.99    | L3_warm_100k_zipf99       | L3_50r_zipf99 |
| L3 | zipf θ=1.2     | L3_hot_100k_zipf12        | — |
| L3 | zipf θ=1.5     | L3_extreme_100k_zipf15    | — |

Write-heavy stays at uniform + warm only (8 cells skipped). Adding hot/extreme write-heavy variants would 2× the write-heavy section for marginal additional signal — easy to add later if a specific question demands them.

**Sweep cost** (12 s/trial × 7-point ladder × 3 repeats × 12 workloads):
- wh_compare:       7 locks × 12 × 7 × 3 = **1,764 trials** (~5.9 h)
- cds_sweep:        5 locks × 12 × 7 × 3 = **1,260 trials** matrix + ~159 specials (~5.0 h)
- run_avl_compare:  10 (5 × 2) × 12 × 7 × 3 = **2,520 trials** (~8.4 h)

`--quick` mode (5 s × 1 repeat) ≈ 12× faster.

## 2026-05-10 — D22: Pre-rolled per-thread (key, op) streams

**Decision.** Per-op RNG/sampling cost — `mt19937_64` + `mt19937` + (zipfian: `std::pow` + FNV-1a + modulo) — moved out of the measurement window. Each worker pre-rolls a fixed-length cyclic stream of `(key, op_code)` entries using the *same* RNG seeds and op-mix logic as the live `do_op`; the hot loop is then a single `load + switch + call` with bitmask-wrap on the index.

**Implementation.**
- New header `include/util/op_stream.hpp` providing `op_entry { uint64_t key; uint8_t op; }` (16-byte aligned), `make_stream_uniform`, `make_stream_zipfian`, and `round_up_pow2`.
- Index harness `bench_harness.hpp::run_bench_common`: collapsed zipfian/uniform branches into a single body that walks the pre-rolled stream. New `--stream_len` flag (default **4096**, 16 B × 4096 = 64 KiB / thread; fits Xeon L2 = 256 KiB cleanly). `idx` is *not* reset between phases — phase 2 inherits the position phase 1 reached. Threads start at staggered offsets `t * (stream_len / threads)` to decorrelate the first 64 ops.
- Microbench `bench/main.cpp`: `make_rw_mask` packs the read/write coin flip into a `vector<uint64_t>` bitstream. `bench_rw`, `bench_occ_rw`, `bench_rcu` use bit-test on the mask; `bench_mutex` is untouched (no RNG to begin with). New `--stream_len` (default **1024**, fits L1 trivially).

**Measured impact (macOS M3, post-build).**

| Bench | Config | ns/op before | ns/op after | Delta |
| --- | --- | ---: | ---: | ---: |
| microbench `rw` (rw_lock) | 4T, 90% read | ~32.6 | 29.5 | −3.1 |
| microbench `rw` (pcpu_rw_lock) | 4T, 90% read | ~21.2 | 22.2 | ~flat |
| wh_bench_pcpu-rw, uniform L3 | 8T, 90/5/5 | (live RNG) | 13.2 | meaningful drop on uniform path |

The microbench gain is small because the live `mt19937 + uniform_int(0,99)` was already only ~5-8 ns/op; the lock+CS dominates. The bigger expected win is on the zipfian index path (live `std::pow + FNV-1a + modulo` was ~50-80 ns/op). Sanity-tested via `wh_test_*` for every lock variant (all PASS) and `locktest` (8/8 PASS including new `pcpu_rw_lock (write)` and `pcpu_rw_lock (mixed)` tests).

**Caveats.**
- Empirical key-mass distribution at small N=4096 may deviate a few % from theoretical Zipf (especially at θ≥1.2 where the top key holds tens of percent of probability mass). This is **shared bias across every lock variant in a sweep**, so lock-vs-lock comparisons remain fair — but the absolute throughput numbers reflect a slightly different distribution than ideal Zipfian sampling at infinite N.
- A short cyclic stream lets the hardware prefetcher and branch predictor enter steady state, which cleans up measurement noise but means the absolute numbers will look ~5-10 % faster than the theoretical "every-op-independent" baseline. This is intentional — every lock variant pays the same prediction benefit.
- The `--stream_len <N>` knob is exposed for empirical validation: a flat-region test across `N ∈ {1024, 4096, 16384}` should show throughput stable within ±2 %.

**Files changed.** `include/util/op_stream.hpp` (new); `include/util/bench_harness.hpp` (params field, parser, `run_bench_common` body); `bench/main.cpp` (params field, parser, `make_rw_mask`, three worker lambdas).

## 2026-05-10 — D22 follow-up: Xeon flat-region check + CSV columns

### Flat-region check on Xeon E5-2650L v3 (no longer macOS-noisy)

Ran `wh_bench_pcpu-rw --threads 12 --seconds 10 --warmup 2 --read_pct 90 --insert_pct 5 --dist uniform --key_range 100000 --prefill 50000 --pin_policy compact_phys` with three stream lengths × 3 trials each:

| stream_len | bytes / thread | trials (M ops/s)        | median ns/op | landing |
| ---: | ---: | --- | ---: | --- |
| 1024  | 16 KiB | 87.96, 87.62, 87.39 | **11.4** | fits in L1d (32 KiB Haswell-EP) |
| 4096  | 64 KiB | 81.05, 80.74, 80.66 | 12.4 | fits in L2 (256 KiB) |
| 16384 | 256 KiB | 77.25, 77.34, 77.47 | 12.9 | at L2 boundary |

**Within each stream_len, variance is < 1%** (12 threads × `compact_phys` pinning works as intended — the noise we saw on macOS was entirely M3 P/E-core scheduling, not a stream-design issue). Between stream_lens, there is a **real 12 % cache-fit step**: smaller streams stay in higher cache and the stream walk is ~1 ns/op cheaper.

The flat-region assumption from the initial D22 was wrong on Xeon. Updated framing:

- The cost is real but it's a **uniform shift across all lock variants** consuming the same `stream_len`, so lock-vs-lock comparisons remain valid at any choice.
- **Decision: keep `stream_len = 4096` default.** At θ=1.5, 1024 samples is coarse enough that the second-tier hot keys' empirical mass has visible variance; 4096 gives a better statistical fidelity ↔ throughput trade-off. If the goal is max absolute throughput on Xeon and high-skew workloads aren't a concern, `--stream_len 1024` is a valid override (gains ~10 %).

### CSV columns: `stream_len`, `prefill` now emitted per row

The flat-region check above initially collapsed all 9 trials into one aggregate row because `stream_len` wasn't in the header. Fixed: `print_bench_result` in `include/util/bench_harness.hpp` now emits `stream_len` and `prefill` as additional columns. `scripts/aggregate.py` picks them up automatically because they aren't in `NON_KEY_COLUMNS` (any column not in that set becomes a grouping key). Backward-compat: old CSVs without these columns still aggregate correctly (the DictReader returns empty strings for missing keys, which group together).

## 2026-05-10 — D23: Cut publishable sweep to ≤ 8 h via 12T cap + 5 s × 3

The full publishable sweep across `wh_compare.sh` + `cds_sweep.sh` + `run_avl_compare.sh` was ~20 h on Xeon. User wanted ≤ 8 h budget without losing any of the three benches. Three cuts get to ~6.6 h with margin:

1. **Single-socket cap on the topology ladder.** `compute_thread_ladder` (`scripts/sweep_common.sh`) now omits the cross-socket and full-machine (SMT) phases. On Xeon E5-2650L v3: `1 2 4 8 12 24 48` → `1 2 4 8 12` (5 points). On single-socket ARM (Apple M3, Graviton): unchanged. Saves ~30 % of trials and produces a cleaner lock-vs-lock story — no cross-socket coherence cost confounding the comparison. Reversible by uncommenting the two trailing `[ ... ] && ladder="..."` lines (D17's cross-socket and SMT phases).

2. **`DEFAULT_SECONDS` 10 → 5.** Post-warmup, 5 s is enough for steady-state measurement once compact_phys pinning + 2 s warmup absorb scheduling jitter. 3 repeats × 5 s = 15 s of measurement per cell is plenty for the aggregator's median+IQR. Saves ~40 % of per-trial time.

3. **Drop AVL ↔ StripedMap redundancy** in `run_avl_compare.sh`. Removed the `cdsbench` loop entirely; the script now only runs `cds_avl_bench` (BronsonAVL). `cds_sweep.sh` already produces the StripedMap matrix data at `results/<arch>/cdsbench/cdsbench.csv` — the comparison notebook joins on (lock, dist, threads, workload) keys. Saves ~30 % of `run_avl_compare`'s wall-clock and eliminates duplicated trials.

**Math** (5 s × 3 = 7.5 s/trial, 5-point ladder on Xeon):

| Script | Trials | Wall-clock |
| --- | ---: | ---: |
| `wh_compare.sh` (7 × 12 × 5 × 3) | 1,260 | ~2.6 h |
| `cds_sweep.sh` matrix (5 × 12 × 5 × 3) | 900 | ~1.9 h |
| `cds_sweep.sh` specials | ~129 | ~16 min |
| `run_avl_compare.sh` (5 × 12 × 5 × 3, AVL only) | 900 | ~1.9 h |
| **Total** | **3,189** | **~6.6 h** |

What stays the same: all 12 workloads including θ=1.5 extremes; `compact_phys` pinning; prefill pinned to socket 0; pre-rolled `(key, op)` streams; ARM topology ladders unchanged on Apple M3 / Graviton.

**Trade-off accepted.** Loses NUMA cross-socket coherence data (the 12 → 24 jump) and SMT-contention data (24 → 48) on Xeon. D17's "topology-aware ladder" rationale was to expose those effects; we've decided the cleaner single-socket lock-vs-lock story is more valuable for the thesis question. Both are recoverable by reverting the two-line comment in `compute_thread_ladder`.

## 2026-05-11 — D24: `pcpu_rw_lock` collapse — diagnosis confirmed (thundering herd)

Notebook §6 hypothesised the per-CPU rwlock fails on Graviton2 via a writer-induced reader retract storm. A targeted thread-sweep + read-percentage sweep on Graviton2 confirmed this conclusively:

- **Thread sweep at 90/5/5:** smooth feedback degradation 1T→6T (17.3 → 0.15 M ops/s), with trial-to-trial variance growing monotonically. **Not** a discrete cliff.
- **Read-percentage sweep at 4T:** at 100/0/0 the lock scales super-linearly to 75.6 M ops/s (4.37×). One percent writers (99/0/1) immediately costs 43 % of throughput; 5 % writers cost 94 %.

**Verdict:** the per-CPU slot infrastructure is sound (super-linear read scaling proves no reader contention). The retract-and-spin reader protocol is the bug — any non-trivial writer rate triggers the herd.

**Decision:** investigation tracked in `docs/INVESTIGATION_PCPU_RW.md`. Fix candidates: (a) Linux `percpu_rwsem`-style commit-don't-retract protocol (recommended); (b) BRAVO biased rwlock; (c) reader backoff (one-line fallback). Prototype (a) as `pcpu_rw_lock_v2` in a separate primitive so the original measurements stay intact for the comparison.

Not blocking the thesis writeup — the current data is a valid finding ("naïve per-CPU rwlock collapses under contention; per-leaf seqlocks (`wh-occ-opt`) dominate the alternative"). The v2 prototype, if it works, *strengthens* the story rather than replacing it.

## 2026-05-11 — D25: `pcpu_rw_lock_v2` validated; fix loop closed

The `percpu_rwsem`-style fix (D24 Option A) was implemented as `include/primitives/pcpu_rw_lock_v2.hpp` and validated on Graviton2 against v1 on the workload that broke v1 (L1_warm_zipf99 read-heavy 90/5/5):

| threads | v1 median (M/s) | v2 median (M/s) | v2/v1 |
| ---: | ---: | ---: | ---: |
| 4 | 4.90 | 30.35 | 6.2× |
| 8 | 0.03 | 16.00 | **533×** |

v2 scales 1T→5T (16.5 → 31.6 M/s), then plateaus and declines (8T = 16.0 M/s) — a much milder secondary bottleneck (`writer_mu` saturation under 10 % writers × 8 threads), not the thundering-herd catastrophe. Trial variance is gone (8T trials: 14.6 / 16.9 / 16.0, vs v1's 38.6 K / 32.2 K / 11.2 K).

**At 8T on Graviton, v2 sits between `wh-default` and the spinlocks** (16.0 M/s vs default 20.3 M/s vs spinlocks ~14–15 M/s). Lock-free `wh-occ-opt` remains the dominant winner at 69.6 M/s.

The investigation loop (diagnose → hypothesise → fix → validate) is closed. Full record in `docs/INVESTIGATION_PCPU_RW.md`. Thesis story strengthens: "naïve per-CPU rwlock fails catastrophically; the percpu_rwsem-style fix restores it to mid-pack throughput; lock-free OCC reads remain the practical winner for read-heavy concurrent indexes."

Follow-ups (not blocking): full `wh_compare.sh` sweep with v2 across the workload matrix on both arches; notebook §6 update to incorporate the validation data.
