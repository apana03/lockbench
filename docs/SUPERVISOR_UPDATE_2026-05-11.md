# Supervisor update — week of 2026-05-04 → 2026-05-11

Changes since the previous meeting (the `index_lock_evaluation.pptx` deck on 2026-05-04). Skips project context — picks up where we left off.

## TL;DR

- Rewrote the benchmark harness (removed RNG and atomic-load overhead, added per-CPU rwlock, unified the workload matrix across all three benches and both arches).
- Full publishable sweep now runs in ~6.6 h instead of ~20 h. Ran it end-to-end on **both Xeon and Graviton2** this week.
- Notebook rebuilt as a cross-platform analysis with microarchitectural interpretation and cited sources.
- One genuinely surprising result triggered a full investigation arc this Monday — **diagnose → hypothesise → fix → validate**, in a single day. The naïve per-CPU rwlock collapses on Graviton2. The `percpu_rwsem`-style fix recovers it. The investigation is closed.

## What I changed this week

Five distinct pieces of work, in chronological order — the order matters because each one unblocks the next.

### 1.  Harness overhead reduction + new primitive (commit `aa3a8e2`, D22)

Two problems I noticed after running the previous sweep:

- **RNG was in the timed window.** Per-op `mt19937_64` key sampling + `mt19937` op-coin-flip + `std::pow` + FNV-1a was costing ~5–80 ns/op inside the measurement. For locks with 30–50 ns critical sections, the RNG was a meaningful fraction of measured time.
- **Per-op `atomic.load(measuring)` + `atomic.load(stop)`** also lived in the hot path.

Fix: each worker pre-rolls a fixed-length cyclic stream of `(key, op_code)` entries up front using the same RNG/seeds. Hot loop is now `load + switch + call` with bitmask-wrap on the index. Strided `stop` check every 64 iters takes the second atomic out.

While I was rewriting the harness I also:

- **Added `pcpu_rw_lock` as a new primitive** (`include/primitives/pcpu_rw_lock.hpp`). 64 slots, one cache line per slot. Per-thread reader counter on its own cache line — the predicted "fix" for the counter-based rwlock's cache-coherence bottleneck. Wired into wormhole via the lock shim and into the microbench.
- **Unified the workload matrix** across `wh_compare.sh`, `cds_sweep.sh`, `run_avl_compare.sh` via `scripts/sweep_common.sh`. Same 12-cell matrix (cache regime × skew × mix), same pinning, same trial budget. The only axis that varies between the three benches is the lock list (wormhole has rwlock variants; StripedMap and BronsonAVL are exclusive-only).

### 2.  D22 follow-up: validated the stream design on Xeon (commit `7a13625`)

I'd assumed the cyclic-stream design would behave the same on Xeon and ARM. Tested it: ran `wh_bench_pcpu-rw` at 12T on Xeon with three stream lengths × 3 trials each.

| stream_len | bytes/thread | median (M ops/s) | landing in cache |
| ---: | ---: | ---: | --- |
| 1024 | 16 KiB | 87.7 | L1d |
| 4096 | 64 KiB | 80.8 | L2 |
| 16384 | 256 KiB | 77.4 | L2 boundary |

**Within each stream_len, variance is < 1 %.** Pinning works. **Between stream_lens there's a real 12 % step** — smaller streams stay in higher cache and the stream walk is ~1 ns/op cheaper. The shift is uniform across all locks so lock-vs-lock comparisons remain valid, but I picked `stream_len = 4096` as the default because it gives better statistical fidelity at θ=1.5 than the L1d-fitting 1024 case. Also pushed `stream_len` and `prefill` into the CSV header so the aggregator picks them up automatically.

### 3.  Cut the publishable sweep to ≤ 8 h (commit `081dfed`, D23)

Pre-cut sweep was ~20 h across `wh_compare.sh` + `cds_sweep.sh` + `run_avl_compare.sh` on Xeon. Three cuts got us to ~6.6 h:

- **Single-socket cap on the topology ladder.** Xeon goes `1 2 4 8 12` instead of `1 2 4 8 12 24 48`. Loses the cross-socket and SMT phases but the lock-vs-lock story stays clean (no NUMA coherence confounding). Two-line revert if we want NUMA back.
- **`DEFAULT_SECONDS` 10 → 5 s.** Post-warmup, 5 s is enough for steady-state once `compact_phys` pinning absorbs jitter. 3 × 5 s of measurement is plenty for median + IQR.
- **Dropped the AVL-vs-StripedMap redundancy** in `run_avl_compare.sh` — it was running cdsbench *again* even though `cds_sweep.sh` already produces that data.

### 4.  Ran the full publishable sweep on both archs (commits `075ccd6`, `875901b`)

- Xeon E5-2650L v3: full 12-workload × 5-thread × 3-trial × 3-bench matrix, ~6.6 h.
- Graviton2: same matrix, ~3 h (single socket, 8 cores).

Aggregates committed at `results/<arch>/{wh_compare, cdsbench, avl_compare}/*.agg.csv` — these are what the notebook reads.

### 5.  Notebook rebuilt as cross-platform analysis (commits `33e413f`, `896120d`)

The previous notebook (`index_cross_arch.ipynb`) compared raw throughput numbers but didn't dig into the microarchitectural reasons for the cross-arch gap. The new one (`scripts/lockbench_analysis.ipynb`) does this:

- 14 sections; the through-line is "every cross-arch ratio decomposes into clock + DRAM + LSE atomics, and the residual tells us what each lock is bottlenecked on."
- All claims now have primary-source citations: ARM ARM (DDI 0487) for LSE, *Cortex-A Series Programmer's Guide* §13 for LSE-vs-LL/SC cycle costs, Intel SDM Vol. 3A §8.1 for LOCK semantics, Calciu et al. (PPoPP 2013) for the counter-based rwlock cost model, David et al. (SOSP 2013) for cross-ISA atomic-latency baselines, Hennessy & Patterson 6e §2.7 for the memory-wall scaling argument.

Built from `scripts/_build_lockbench_analysis.py` so the notebook stays reproducible.

### 6.  The `pcpu_rw_lock` investigation (commits `cd234c6` → `07cae5d` → `8bbb40d` → `cb3662b`, D24 → D25)

This is the part I want to spend the most time on. When the publishable sweep came back, the new `wh-pcpu-rw` primitive — the one I'd predicted would be the win — was doing *catastrophically badly* on Graviton2 at high thread counts. Worse than the lock it was supposed to replace, and worse on Graviton than on Xeon (inverting the cross-arch story for every other lock).

| arch | 1T | 4T | 8T |
| --- | ---: | ---: | ---: |
| Xeon | 13.6 M | 18.8 M | 10.9 M |
| Graviton | 17.2 M | **4.9 M** | **0.03 M** |

I treated this as an investigation: write it up in `docs/INVESTIGATION_PCPU_RW.md` as I went.

**Diagnostic 0a — thread sweep at 90/5/5.** Smooth feedback degradation 1T → 6T (17.3 → 0.15 M ops/s), trial variance grows monotonically. *Not* a discrete cliff — a feedback curve, consistent with a thundering-herd hypothesis.

**Diagnostic 0b — read-percentage sweep at 4T.** The clean test: hold thread count fixed, vary writers.

| read_pct | median M ops/s | vs 1T baseline (17.3) |
| ---: | ---: | ---: |
| 100 (no writers) | 75.6 | **4.37× — super-linear** |
| 99 (1 % writers) | 43.0 | 2.49× |
| 95 (5 % writers) | 4.87 | 0.28× |
| 90 (10 % writers) | 4.77 | 0.28× |

A **one-percent** writer rate costs 43 % of throughput. Five percent costs 94 %. The per-CPU slot design *works* (super-linear at 100 % reads — that's the whole point). The reader–writer handshake is the bug.

**Diagnosis.** Reader fast path is `fetch_add(slot.count)` → check `writer_present` → if writer present, `fetch_sub` (retract) and spin. When a writer arrives, every concurrent reader retracts simultaneously and all spin. The writer scans all 64 slot counters waiting for them to drain — but as readers retract, others retry the moment `writer_present` clears, so the next writer arrival finds a full house again. The system oscillates.

**Why worse on Graviton.** Graviton2's LSE atomics make the readers' retry loop *tighter* — they all complete `fetch_add` within nanoseconds of each other, giving the writer a full house of readers to drain on every cycle. Xeon's slower `LOCK XADD` accidentally throttles the herd by spreading the readers out. Faster atomics make the pathology worse. This is the most novel finding in the project so far.

**Diagnostic 0c — microbench cross-check.** Ran the bare primitive in `bench/main.cpp` with no wormhole call pattern. Lock fails to scale on its own (1T → 6T loses 79 %), but milder (6T = 12.9 M vs wormhole-amplified 0.15 M). The bare primitive is the root cause; wormhole amplifies it ~86× via (a) two locks per op (metalock + leaflock both rwlocks) and (b) writer CS hundreds of ns long vs 10 ns in microbench.

**Fix — `pcpu_rw_lock_v2`.** Implemented Linux `percpu_rwsem`-style semantics: readers **commit** rather than retract; writers queue on a slow-path mutex and drain readers without forcing them off. Same per-thread slot infrastructure (the data layout was right). Only the writer–reader handshake changed. Header is `include/primitives/pcpu_rw_lock_v2.hpp`; wired into wormhole and microbench; correctness tests pass (8 threads × 20 000 ops race test, mutual-exclusion + mixed-read/write torn-read).

**Validation on Graviton2.** Same workload that broke v1:

| threads | v1 median (M/s) | v2 median (M/s) | v2 / v1 |
| ---: | ---: | ---: | ---: |
| 1 | 17.32 | 16.48 | 0.95× |
| 4 | 4.90 | 30.35 | **6.2×** |
| 5 | 0.41 | 31.60 | **77×** |
| 8 | 0.030 | 16.00 | **533×** |

v2 scales 1T → 5T (16.5 → 31.6 M/s) then plateaus and gently declines through 8T — a much milder secondary bottleneck (writer-side mutex saturation under 10 % writers × 8 threads), *not* the catastrophic herd. Trial variance gone (8T trials are 14.6, 16.9, 16.0 — vs v1's 38 K, 32 K, 11 K).

**Cost of v2 at 1T: ~5 % regression** (one extra branch on the fast path). Obviously worth it.

The diagnose → hypothesise → fix → validate loop ran in one day. Investigation is closed.

## What the new data says

Three findings from the notebook that I'd want to walk through. The deck (`results/deck/2026-05-11_cross_arch_findings.pptx`) is structured around these.

1. **`wh-occ-opt` (lock-free reads via per-leaf seqlock) is the categorical winner on read-heavy workloads on both archs.** 3.4× over `wh-default` on Graviton, 3.7× on Xeon at max threads. Readers never touch the lock state, so they pay no atomic cost and no coherence cost. If the data structure permits OCC, you should use it.

2. **Graviton2 is 1.4–2.8× faster than Xeon on every well-behaved lock.** The microarchitectural decomposition lines up: spinlock variants get ~2–3× (atomic-bound — LSE atomics win); `wh-default` plateaus at ~1.5× (coherence-bound on a contended counter line); `wh-occ-opt` sits at ~1.3× (memory-bound on the leaf walk). Each ratio fingerprints the lock's bottleneck.

3. **The pcpu_rw_lock story is the central methodological finding.** Naïve per-CPU rwlocks are not a drop-in fix for counter-based rwlocks — the protocol matters more than the data layout. Faster atomics can make a fragile lock fail harder. The same code that limps along on a 2014 Xeon catastrophically breaks on a 2020 Graviton. Hardware modernisation can expose latent timing bugs.

## Open questions for you

- **NUMA story.** Currently dropped via D23's single-socket cap. Recovering the cross-socket (12 → 24) and SMT (24 → 48) phases on Xeon adds ~3.3 h of sweep. Two-line revert. Worth it for the thesis or skip?
- **v2 across the full matrix.** The v2 validation table above is only the diagnostic cell on Graviton. Want to rerun `wh_compare.sh` with v2 in the lock list across all 12 workloads on both arches (~6.6 h × 2 archs).
- **Notebook §6** still reads "anatomy of a thundering herd" without showing the v2 resolution. Want to update it to the full diagnose → fix → validate arc, but waiting for the full v2 sweep first.
- **Higher thread counts on Graviton.** c6g.16xlarge would give us 16+ cores. Don't think the thesis needs it (the v2 fix is validated; v1's collapse is documented) but flagging in case you do.

## References

- New decisions in this period: D22, D23, D24, D25 in `docs/INDEX_LOCK_DECISIONS.md`.
- Full investigation journal: `docs/INVESTIGATION_PCPU_RW.md`.
- Notebook: `scripts/lockbench_analysis.ipynb`.
- This meeting's deck: `results/deck/2026-05-11_cross_arch_findings.pptx`.
- Speaker notes: `docs/SUPERVISOR_DECK_NOTES.md`.
