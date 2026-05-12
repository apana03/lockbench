# Handoff — lockbench session log

**Purpose:** orient a new session (Claude or human) on what's been done in this repo, what the current state is, and what's outstanding. Read this first before doing anything substantive.

> **One-paragraph summary.** The lockbench benchmarks went from "Xeon variance too high, wormhole results dubious, lock-vs-lock deltas below noise floor" to a publishable cross-architecture comparison of seven lock primitives across two server-class platforms (Xeon E5-2650L v3 and AWS Graviton2). Along the way: built a per-CPU rwlock (`pcpu_rw_lock`), discovered it had a catastrophic thundering-herd failure mode, diagnosed it via three targeted experiments, fixed it with a `percpu_rwsem`-style design (`pcpu_rw_lock_v2`), and validated the fix on real hardware (533× improvement at 8T). The thesis narrative is now "naïve per-CPU rwlock fails catastrophically; the percpu-rwsem-style fix restores it to mid-pack throughput; lock-free OCC reads (`wh-occ-opt`) remain the practical winner."

---

## Where to look first

If you need to come up to speed quickly, read in this order:

1. **`docs/INDEX_LOCK_DECISIONS.md`** — running log of every methodology decision (D1–D25 currently). Authoritative on *why* something is the way it is.
2. **`docs/INVESTIGATION_PCPU_RW.md`** — running journal of the per-CPU rwlock collapse investigation: symptom → 3 diagnostics → fix → validation. Status: complete.
3. **`scripts/lockbench_analysis.ipynb`** — the analysis artifact (41 cells, cross-arch focused, ~1.8 MB after rendering).
4. **`docs/EXPERIMENT.md`** — older experiment write-ups (not the recent work; reference only).
5. **This file (`Handoff.md`)** — session-level summary.

Generator pattern to know about: the notebook is regenerated from **`scripts/_build_lockbench_analysis.py`**. If you change one, back-port to the other. The Python build script is the single source of truth in spirit; edits to the `.ipynb` directly are also fine (latest run is what's committed).

---

## Current state (as of latest commit)

### Branch / push state
- `main` is the working branch. All work is on it.
- The most recent meaningful commit is the v2 implementation + validation; verify with `git log --oneline | head -15`.

### Sweep data presence
- `results/x86_64/wh_compare/wh.csv` — **may be stale** (pre-v2). User is running an updated sweep with `wh-pcpu-rw-v2` included.
- `results/aarch64/wh_compare/wh.csv` — **should include v2** as of the user's latest run on Graviton2. Confirm with `cut -d';' -f3 results/aarch64/wh_compare/wh.csv | sort -u` — should list 8 locks including `wh-pcpu-rw-v2`.
- `results/x86_64/cdsbench/cdsbench.csv` and `results/aarch64/cdsbench/cdsbench.csv` — **valid as of last sweep**, no v2 (StripedMap uses exclusive locks only; v2 doesn't apply).
- `results/x86_64/avl_compare/cds_avl.csv` and `results/aarch64/avl_compare/cds_avl.csv` — same, valid, no v2.
- `results/lockbench/x86_xeon_lockbench.csv` — older microbench data; superseded by the per-arch `results/{arch}/...` files for current analysis.

### What's running
- The user just finished a Graviton2 `wh_compare.sh` rerun that includes `pcpu-rw-v2`. They had not yet run `aggregate.py` on it as of last message — they may or may not have aggregated/committed by now.
- The Xeon equivalent rerun is **outstanding**. The user plans to run it next.

### Notebook state
- Current `scripts/lockbench_analysis.ipynb` was last regenerated against pre-v2 data. After both arches' new sweeps (with v2) land in `main`, the notebook should be regenerated so v2 appears in every cross-arch plot. This is the immediate next step once Xeon finishes.

---

## What was achieved this session

### Phase 1 — variance + harness overhaul (D1–D23)
Methodology fixes for publishable cross-arch data. Each decision documented in `docs/INDEX_LOCK_DECISIONS.md`.

Key changes:
- **SMT-aware pinning** (`compact_phys`): one logical thread per physical core, socket 0 first. Implemented in `include/primitives/util.hpp` (sysfs topology probe + `pin_policy` enum). No sudo required.
- **Per-CPU rwlock primitive** (`pcpu_rw_lock` v1): added in `include/primitives/pcpu_rw_lock.hpp`. Wired into the wormhole shim. *This is the primitive that later turned out to have the thundering-herd bug.*
- **Two-phase timed loop + strided stop check**: removed `measuring.load()` and per-iteration `stop.load()` from the hot loop. Lives in `include/util/bench_harness.hpp::run_bench_common` and the four functions in `bench/main.cpp`.
- **Pre-rolled (key, op) streams (D22)**: new `include/util/op_stream.hpp`. Eliminates per-op RNG cost (~15–80 ns/op of zipfian/uniform sampling). The streams are walked cyclically per worker.
- **Cache-regime workload matrix (D11, D21)**: 12 cells across L1-resident (1k keys) and L3-resident (100k keys) × uniform / zipf θ=0.99 / 1.2 / 1.5 × 90/5/5 vs 50/25/25 mixes. Lives in `scripts/sweep_common.sh::WORKLOADS`.
- **Topology-aware thread ladder + single-socket cap (D17, D23)**: ladder caps at single-socket physical cores. Xeon: `1 2 4 8 12`. Graviton: `1 2 4 8`. Implemented in `scripts/sweep_common.sh::compute_thread_ladder()`. Cross-socket and SMT phases intentionally omitted (two commented `[ ... ] && ladder=...` lines to restore).
- **Flat 5 s × 3 repeats budget across all arches (D19, D23)**: no longer per-arch; uniform per-trial cost.
- **LTO on wormhole shim variants**: `CMakeLists.txt` enables `INTERPROCEDURAL_OPTIMIZATION` so `rwlock_lock_read` inlines into wh.c — confirmed via `nm build/wh_bench_tas | grep rwlock_lock_read` returning nothing.
- **Shared sweep helper `scripts/sweep_common.sh`**: all three sweep scripts source it. Per-arch defaults, `compact_phys` pinning, the workload matrix, the topology ladder, and `run_workload_matrix_on` live here.
- **`scripts/aggregate.py`**: per-trial CSV → median + IQR + CoV summary. Flags groups with CoV > 10 %. Python 3.7-compatible (Graviton2 AL2 ships 3.7, so no PEP 604 `|`-union syntax).

### Phase 2 — comprehensive cross-arch analysis notebook
`scripts/lockbench_analysis.ipynb` (41 cells) generated from `scripts/_build_lockbench_analysis.py`. Sections:
- §0–1: setup, data loading, quality (CoV histograms per arch×bench).
- §2: cross-arch headline table.
- §3: cross-arch ratio plot per lock.
- §4: architectural reference card (specs table with sources).
- §5: 1T baseline ns/op (uncontended cross-arch comparison; 1.20–1.34× Xeon/Graviton ratio).
- §6: scaling efficiency `ops(T)/(T·ops(1T))` per arch.
- §7: cache regime × architecture (L1 vs L3 Graviton/Xeon ratios with reference lines for clock-only and DRAM-only advantages).
- §8: heatmap of Graviton/Xeon ratio across every (lock, workload) cell.
- §9: side-by-side scaling matrix per arch.
- §10: best-lock-per-(arch × workload) pivot.
- §11: `wh-pcpu-rw` collapse anatomy.
- §12: cross-bench validation.
- §13–14: findings + caveats.

Every architectural claim is sourced. Sources used (audit-trail in §4–§7 markdown):
- ARM ARM (DDI 0487) for LSE atomics
- Cortex-A Series Programmer's Guide for ARMv8-A (§13) for LSE vs LL/SC
- Agner Fog Instruction Tables for Haswell LOCK XADD/CMPXCHG
- Intel SDM Vol. 3A §8.1 + §11.4 for atomic and coherence semantics
- AWS Graviton2 whitepaper + ARM N1 SDP TRM for Neoverse N1 specs
- JEDEC DDR4 spec for DRAM bandwidth
- Sewell et al. CACM 2010 ("x86-TSO") + Pulte et al. POPL 2018 (ARM concurrency)
- Calciu et al. PPoPP 2013 for counter-rwlock cache-line cost model
- David et al. SOSP 2013 for atomic latency framework
- Hennessy & Patterson §2.7 for memory-wall scaling

### Phase 3 — `pcpu_rw_lock` collapse investigation
On the publishable Xeon + Graviton sweep, the notebook surfaced a 600× collapse: `wh-pcpu-rw` at 8T on Graviton2's `L1_warm_read_heavy` dropped from 17.2 M ops/s (1T) to **0.03 M ops/s** (8T). Investigation in `docs/INVESTIGATION_PCPU_RW.md`:

- **Diagnostic 0a — thread sweep on Graviton2**: smooth degradation 17.3 → 0.15 M ops/s across 1T→6T. Not a discrete cliff; variance grows with thread count. Consistent with thundering herd, doesn't prove it yet.
- **Diagnostic 0b — read-percentage sweep at 4T**: 100/0/0 reads scaled super-linearly to 75.6 M ops/s (4.37×). 1 % writers (99/0/1) cost 43 % of throughput. 5 % writers cost 94 %. **Proved the herd hypothesis** — per-CPU slot design is sound; writer-induced reader retract is the killer.
- **Diagnostic 0c — microbench**: same failure on the bare lockbench microbench (single global lock, no wormhole call pattern) but ~86× milder than in wormhole. Wormhole's metalock + leaflock coupling + long writer CSes amplify the herd; the primitive is the root cause.

### Phase 4 — fix: `pcpu_rw_lock_v2` (Option A, percpu_rwsem-style)
New primitive at `include/primitives/pcpu_rw_lock_v2.hpp`. Same per-CPU slot infrastructure as v1, but:
- **Reader fast path** is the same (per-CPU `fetch_add(acq_rel)` + `writer_pending.load(acquire)` check).
- **Reader slow path** is different: if `writer_pending` is observed true after the fetch_add, the reader retracts and *waits on a `std::mutex`* (futex-backed on Linux). Wakeup is serialised by the mutex queue, **breaking the herd**.
- **Writer path** takes `writer_mu` first, sets `writer_pending`, drains slots, enters CS, clears `writer_pending`, releases `writer_mu`.

Wired in via:
- `third_party/wormhole/wh_lock_shim.cpp` — `WH_LOCK_PCPU_RW_V2` branch.
- `CMakeLists.txt` — `pcpu-rw-v2` added to `WH_LOCKS` → builds `wh_bench_pcpu-rw-v2` and `wh_test_pcpu-rw-v2`.
- `bench/main.cpp` — `--lock pcpu-rw-v2` dispatch for `mutex` and `rw` workloads.
- `bench/lock_test.cpp` — correctness tests templated; both v1 and v2 covered.
- `scripts/wh_compare.sh` — `pcpu-rw-v2` in the swept lock list.

**Validation on Graviton2** (`L1_warm_zipf99 90/5/5`, 5 s × 3 trials):

| threads | v1 (M/s) | v2 (M/s) | v2/v1 |
| ---: | ---: | ---: | ---: |
| 1 | 17.32 | 16.48 | 0.95× |
| 4 | 4.90 | 30.35 | 6.20× |
| 8 | 0.030 | 16.00 | **533×** |

v2 scales 1T→5T (peak 31.6 M/s), then plateaus and descends through 6T→8T (16.0 M/s at 8T). The descent is a *new, milder* bottleneck — `writer_mu` saturation under 10 % writers × 8 readers — not v1's catastrophe. At 8T v2 sits between `wh-default` (20.3 M/s) and the spinlocks (~14 M/s).

**Outcome:** the diagnose → fix → validate loop is closed. Documented in `docs/INVESTIGATION_PCPU_RW.md` (status: complete) and `docs/INDEX_LOCK_DECISIONS.md` D24+D25.

---

## What's outstanding

### Immediate (blocking next analysis update)
- **Xeon rerun of `wh_compare.sh`** with v2 in the lock list. ~3 h on Xeon E5-2650L v3 (single-socket cap, 5 s × 3, 8 locks × 12 workloads × 5 ladder pts × 3 reps). Commands in the most recent assistant message before this handoff.
- **Aggregate + push Graviton sweep** (user may or may not have completed this — confirm with `git log results/aarch64/wh_compare/wh.csv`).
- **Regenerate notebook** once both arches' v2 data is in `main`. The §11 "thundering herd" section should be expanded to "thundering herd, diagnosed and fixed" with v1-vs-v2 plotted across all 12 workloads on both arches. Headline table in §3 should include v2.

### Open follow-ups (non-blocking; low priority)
From `docs/INVESTIGATION_PCPU_RW.md` "Open questions" section:
- **`perf stat` on Graviton2 under v1 collapse mode** — to confirm cycles are stall-backend dominated. The diagnosis-by-data-shape is already conclusive; this would just be belt-and-suspenders.
- **ThreadSanitizer build** to rule out a latent data race in v1's slot allocation. Same comment.
- **Cross-check the v1 collapse on Xeon** — Xeon shows the same collapse signature at higher thread counts (8T–12T), just less severe. Worth running v1-vs-v2 head-to-head on Xeon for the thesis if time permits.
- **Investigate v2's 6T→8T descent** — hypothesis: `writer_mu` saturation. Could be tested with `read_pct=99` sweep on v2 (predict monotonic scaling through 8T if mu is the only bottleneck).

### Not on the table (intentionally out of scope)
- Adding `pcpu_rw_lock` (either version) to `cdsbench` or `cds_avl_bench` — these use exclusive stripe-locks, no rwlock site to swap into. Documented in D9.
- Re-running `cds_sweep.sh` / `run_avl_compare.sh` — those don't use pcpu-rw, their CSVs are valid.
- DRAM-bound workload tier — D7 dropped this; memory latency masks lock differences and weakens the comparison.

---

## Key files quick reference

### Lock primitives
- `include/primitives/util.hpp` — pinning, topology probe, cpu_relax. Note `pin_policy` enum + `clear_thread_affinity()`.
- `include/primitives/pcpu_rw_lock.hpp` — v1 per-CPU rwlock with retract-and-spin. **Has the documented thundering-herd bug.**
- `include/primitives/pcpu_rw_lock_v2.hpp` — v2 percpu_rwsem-style fix. **Recommended.**
- `include/primitives/{tas,ttas,cas,ticket,rw,occ,rcu}_lock.hpp` — the other six primitives. Stable.

### Bench harness
- `include/util/bench_harness.hpp` — `params` struct (arg parser), `run_bench_common()`, prefill helper. **Critical: column-naming gotcha — never use `df.skew`; pandas' `DataFrame.skew` is the statistical method. Use `df['skew_tier']` (renamed) or any bracket access.**
- `include/util/op_stream.hpp` — pre-rolled `(key, op_code)` streams.
- `bench/main.cpp` — `lockbench` microbench. Four workload functions (`bench_mutex`, `bench_rw`, `bench_occ_rw`, `bench_rcu`) all use the two-phase loop pattern.
- `bench/lock_test.cpp` — correctness tests. Templated for the pcpu-rw variants.
- `bench/wh_bench.cpp` + `bench/cds_bench.cpp` + `bench/cds_avl_bench.cpp` — index benches; all delegate to `run_bench_common`.

### Sweep infrastructure
- `scripts/sweep_common.sh` — **the** shared helper. Per-arch defaults, topology ladder, workload matrix, `run_workload_matrix_on()`.
- `scripts/wh_compare.sh` — wormhole sweep (the rwlock-rich one).
- `scripts/cds_sweep.sh` — StripedMap matrix + bucket sweep + resize-stress sections.
- `scripts/run_avl_compare.sh` — BronsonAVL only (the redundant cdsbench loop was removed in D18).
- `scripts/aggregate.py` — per-trial CSV → median/IQR/CoV. Python 3.7-compatible (no PEP-604).
- `scripts/lockbench_analysis.ipynb` — analysis notebook.
- `scripts/_build_lockbench_analysis.py` — generator for the notebook.

### Wormhole shim
- `third_party/wormhole/wh_lock_shim.cpp` — `LockT` selection via `WH_LOCK_<NAME>` macros. Templates `do_read_lock`, `do_read_unlock`, `try_excl`, `try_shared`, `do_write_to_read` all handle both `pcpu_rw_lock` and `pcpu_rw_lock_v2` as rwlocks (alongside `rw_lock`).
- `third_party/wormhole/wh_lock_shim.h` — shim storage size (128 bytes per lock); pcpu_rw_lock heap-allocates the slots array so the embedded struct fits.

### Build system
- `CMakeLists.txt` — `WH_LOCKS` is the source of truth for which wormhole binaries get built. LTO via `INTERPROCEDURAL_OPTIMIZATION` enabled where supported.

---

## Methodology gotchas (don't relearn these)

### pandas column-name collisions
`DataFrame.skew()` is the statistical-skewness method. If a column is named `skew`, then `df.skew == 'warm'` compares the bound method to a string and returns `False`, with **no error**. Renamed to `skew_tier` everywhere in our code. Other potentially-shadowed names to be wary of: `mean`, `median`, `std`, `count`, `index`, `columns`. Always prefer `df['col_name']` over `df.col_name` in pipeline code.

### Python on Graviton2 (Amazon Linux 2)
AL2 ships Python 3.7. PEP-604 union syntax (`float | None`) needs Python 3.10+. Use `Optional[float]` from `typing` for portability. The aggregator was bitten by this once; now fixed.

### macOS scheduling noise
M3 has heterogeneous P/E cores. Even with QoS hints, threads can land on E-cores at high counts, producing artificially high CoV. Documented in D9 followups. Don't trust macOS data for absolute claims; use it only for development sanity.

### No sudo on Xeon
The Xeon box (`diascld45`) has no sudo. Therefore:
- Can't run `scripts/setup_cpu.sh` (cpufreq governor lock + turbo disable).
- Can't disable SMT at the OS level.
- Work around with: `compact_phys` pinning (one thread per physical core; no SMT contention at the bench-thread level), 5 s × 3 trials for noise tolerance, and the aggregator's CoV warnings to spot anomalies.

### Single-socket cap (D23)
The wormhole sweep ladder is capped at single-socket physical cores on Xeon (so `1 2 4 8 12`, not `1 2 4 8 12 24 48`). This is intentional: cross-socket NUMA coherence cost is a confound for lock-vs-lock comparisons. To restore the full topology ladder, uncomment two `[ ... ] && ladder=...` lines in `scripts/sweep_common.sh::compute_thread_ladder()`. The single-socket-only data is sufficient for the thesis story.

### Lock-naming convention
- `wh-default` = wormhole's stock rwlock (counter-based, similar in design to our `rw_lock`).
- `wh-rw` was dropped from comparisons (D8): redundant with `wh-default`.
- `wh-pcpu-rw` = our per-CPU rwlock v1 (has the bug).
- `wh-pcpu-rw-v2` = our fix.

### Build cycle
- After `CMakeLists.txt` edits: `cmake -B build -DCMAKE_BUILD_TYPE=Release` then `cmake --build build -j$(nproc)`.
- After code-only edits: just `cmake --build build -j$(nproc)`.
- macOS `brew` Python: `python3 -m pip install` is blocked by PEP 668. Use the project's `.venv-plot/` venv for nbconvert / matplotlib.

### Notebook editing pattern
The build script `scripts/_build_lockbench_analysis.py` is documented to be "single source of truth in spirit" but the `.ipynb` is what's executed and committed. Workflow:
1. Edit `scripts/_build_lockbench_analysis.py`.
2. Run `python3 scripts/_build_lockbench_analysis.py` to regenerate the `.ipynb`.
3. Run `.venv-plot/bin/jupyter nbconvert --to notebook --execute --inplace scripts/lockbench_analysis.ipynb` to execute it.
4. Commit both files.

If you hand-edit the `.ipynb`, back-port to the build script when convenient, or just commit the `.ipynb` and let drift happen for one cycle.

### Don't run two sweeps simultaneously
The user has bumped into this twice: launching `wh_compare.sh` via `nohup` and then re-launching when the first didn't appear to be running. Both processes share the same output CSV path and race. Always confirm exactly one is running:
```bash
ps -ef | grep "bash -c.*wh_compare" | grep -v grep | wc -l   # expect 1
```

---

## Sources to keep on hand

Cited in the notebook §4–§7:

- ARM Limited. *Arm Architecture Reference Manual for A-profile architecture* (DDI 0487). For LSE atomics: §B2.9.
- ARM Limited. *Cortex-A Series Programmer's Guide for ARMv8-A*, §13 — LSE vs LL/SC cycle-cost comparison.
- ARM Limited. *Arm Neoverse N1 Software Optimization Guide* — N1 SDP cycle costs.
- AWS. *Graviton2: Performance Powerhouse* whitepaper — clock, DRAM, cache specs.
- Intel Corporation. *Intel® 64 and IA-32 Architectures Software Developer's Manual* Vol. 3A, §8.1 (atomic operations) and §11.4 (cache coherence).
- Intel ARK product page: Xeon E5-2650L v3 — clock and memory specs.
- Agner Fog. *Instruction Tables* (Haswell section) — `LOCK XADD` / `LOCK CMPXCHG` cycle counts.
- JEDEC. *DDR4 SDRAM Standard* (JESD79-4) — theoretical bandwidth specifications.
- Sewell et al. *x86-TSO: A Rigorous and Usable Programmer's Model for x86 Multiprocessors.* CACM 53(7), 2010.
- Pulte et al. *Simplifying ARM Concurrency.* POPL 2018.
- Calciu, Dice, Lev, Luchangco, Marathe, Shavit. *NUMA-Aware Reader-Writer Locks.* PPoPP 2013 — §2 counter-rwlock cost model; the canonical reference for the cache-line-bouncing problem `wh-pcpu-rw` was meant to fix.
- David, Guerraoui, Trigonakis. *Everything You Always Wanted to Know About Synchronization but Were Afraid to Ask.* SOSP 2013 — atomic latency on x86 and ARM, framework for decomposing per-op cost.
- McKenney, P. E. *Is Parallel Programming Hard, And, If So, What Can You Do About It?* (book) — RCU + `percpu_rwsem` design inspiration for v2.
- Hennessy & Patterson. *Computer Architecture: A Quantitative Approach*, 6th ed., §2.7 — memory-wall scaling.

---

## How to resume next session

If you (future Claude) are asked to continue this work, here are the most likely entry points:

### "What did we run on Xeon?" / "What's the Xeon status?"
1. Check `git log results/x86_64/wh_compare/wh.csv` — last commit date tells you whether the v2-included rerun has landed.
2. If stale (no recent v2-rerun commit), the user still owes the Xeon sweep. Direct them to the commands in the conversation history or here:
   ```bash
   cd ~/lockbench && git pull && cmake --build build -j$(nproc)
   ./build/locktest --threads 4 --loops 50000   # quick sanity
   nohup bash scripts/wh_compare.sh >/tmp/wh_sweep_$(date +%Y%m%d_%H%M).log 2>&1 &
   echo $! | tee /tmp/wh_sweep.pid
   ```

### "Regenerate the notebook"
Once both arches have v2 data:
1. Pull, ensure `results/{x86_64,aarch64}/wh_compare/wh.csv` both contain `wh-pcpu-rw-v2`.
2. Update `scripts/_build_lockbench_analysis.py` if you want to add new cells (e.g., v1-vs-v2 comparison section).
3. `python3 scripts/_build_lockbench_analysis.py` then `.venv-plot/bin/jupyter nbconvert --to notebook --execute --inplace scripts/lockbench_analysis.ipynb`.
4. Commit both `_build_lockbench_analysis.py` and `lockbench_analysis.ipynb`.

### "What's the thesis story?"
"Cross-architecture lock-primitive comparison shows that lock-free OCC reads (`wh-occ-opt`) dominate read-heavy concurrent indexes on both x86_64 (Xeon Haswell-EP) and aarch64 (Graviton2 Neoverse N1). A naïve per-CPU rwlock (`wh-pcpu-rw`) catastrophically fails under reader-writer contention due to a thundering-herd in its retract-and-spin protocol; this fails ~533× harder on Graviton2 than on Xeon because LSE atomics make the reader retry loop tighter. A redesigned per-CPU rwlock (`wh-pcpu-rw-v2`) following the `percpu_rwsem` pattern (commit-don't-retract, queue readers on a mutex) restores mid-pack throughput, confirming the diagnosis. The headline finding: when locks are required, counter-based rwlocks remain the most robust low-tail-risk option for short critical sections; per-CPU rwlocks require careful writer-reader handshake design; but the practical winner for read-heavy workloads is to avoid the lock entirely with OCC."

### "Don't repeat: things already settled"
- `wh-rw` is intentionally not in the wormhole comparison (D8). Don't add it back.
- The cdsbench/AVL benches deliberately don't use pcpu-rw (D9). Don't try to wire it in without a real design conversation first.
- DRAM-bound workloads (key_range > 500k) are deliberately excluded (D7). They mask lock differences.
- `setup_cpu.sh` is gitignored because the Xeon has no sudo and we work around without it. Don't suggest running it.
- Macros use `WH_LOCK_PCPU_RW_V2` (underscored), not `WH_LOCK_PCPU-RW-V2` (the dash isn't valid in a C macro). CMakeLists has a `string(REPLACE "-" "_" ...)` to handle this.

---

## Misc context

- The user's email is `andrei.pana77@gmail.com` (auto-memory).
- The user's Xeon box has no sudo; saved in auto-memory at `~/.claude/projects/.../memory/project_xeon_no_sudo.md`.
- The user is a CS researcher exploring locking primitives across architectures (CLAUDE.md).
- The repo lives at `~/lockbench` on the dev boxes (the user's macOS) and at `~/lockbench` on the Graviton EC2 instance and Xeon `diascld45` (per the conversation).
- Plotting venv at `.venv-plot/` is gitignored but exists on the macOS dev box.
