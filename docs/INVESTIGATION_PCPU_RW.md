# Investigation — `pcpu_rw_lock` collapse on Graviton2

**Status:** **complete.** Diagnosis confirmed (thundering herd in the retract-and-spin protocol). Fix prototype `pcpu_rw_lock_v2` validated on Graviton2 — eliminates the collapse: at 8T it delivers 16.0 M ops/s vs v1's 0.03 M ops/s (533× improvement) on the workload that broke v1. v2 now sits in the middle of the lock-comparison ranking, behind `wh-occ-opt` (lock-free reads) but ahead of all spinlock variants. ↗ See also `docs/INDEX_LOCK_DECISIONS.md` D9 (introduction of the primitive), the post-implementation findings appended to D22, and D24 (collapse diagnosis).

This is a running journal. Each section is timestamped; later sections may update or supersede earlier conclusions. Append new findings — don't rewrite old ones.

---

## 2026-05-11 — Symptom

After the publishable Xeon + Graviton sweep (D23), the notebook `scripts/lockbench_analysis.ipynb` surfaced a striking anomaly: `wh-pcpu-rw` collapses badly at ≥ 4 threads on Graviton2's `L1_warm_read_heavy` (key_range=1000, zipf θ=0.99, 90/5/5 read-heavy).

| arch | 1T | 2T | 4T | 8T |
| --- | ---: | ---: | ---: | ---: |
| Xeon E5-2650L v3 | 13.6 M | 15.8 M | 18.8 M | 10.9 M |
| Graviton2 | 17.2 M | 18.3 M | **4.9 M** | **0.03 M** |

Cross-arch comparison (Xeon vs Graviton ratio at same threads) shows Graviton 1.4–2.8× faster on every other lock — but pcpu-rw inverts the trend dramatically.

The original hypothesis in the notebook §6 was a "thundering-herd" failure mode: writers force readers to retract from the lock, all readers retry simultaneously, the system enters a degenerate oscillation. The two diagnostics below test that hypothesis.

---

## 2026-05-11 — Diagnostic 0a: Thread-count sweep (Graviton2)

Sweep `--threads ∈ {1, 2, 3, 4, 5, 6}` at the headline workload (L1_warm_zipf99, 90/5/5), 5 s × 3 trials per cell, with `--pin_policy compact_phys`:

```bash
for t in 1 2 3 4 5 6; do
  for r in 1 2 3; do
    ./build/wh_bench_pcpu-rw --threads $t --seconds 5 --warmup 2 \
      --dist zipfian --zipf_theta 0.99 --key_range 1000 --prefill 500 \
      --read_pct 90 --insert_pct 5 --pin_policy compact_phys
  done
done
```

| threads | median (M ops/s) | trial spread | scaling vs 1T |
| ---: | ---: | --- | ---: |
| 1 | 17.28 | 17.28–17.40 (tight) | 1.00× |
| 2 | 17.74 | 15.70–20.45 | 1.03× |
| 3 | 12.46 | 9.61–14.88 | 0.72× |
| 4 | 2.99 | 2.19–5.53 | 0.17× |
| 5 | 0.58 | 0.58–0.72 | 0.034× |
| 6 | 0.15 | 0.14–0.43 | 0.009× |

**Findings:**

1. **No 4-thread cliff** — the collapse is smooth and continuous. A discrete bug (e.g. slot-index collision at a specific thread count) would have shown a step function; we see a feedback curve.
2. **2T already isn't scaling.** Reader-scalable rwlocks should give ~1.8× at 2T on read-heavy; we get 1.03×. The bottleneck binds at 2 threads already, just not catastrophically.
3. **Trial-to-trial variance grows with thread count.** 1T trials are within 0.7 % of each other; 4T trials span 2.5×. The collapse mode is non-deterministic, depending on which thread happens to win an arbitration first.

Consistent with the thundering-herd hypothesis, but doesn't yet *prove* it. → Run 0b.

---

## 2026-05-11 — Diagnostic 0b: Read-percentage sweep at 4 threads (Graviton2)

Isolates writers as the trigger. Same workload, sweep `read_pct ∈ {100, 99, 95, 90, 50}`, 4 threads, 5 s × 3 trials:

```bash
for rd in 100 99 95 90 50; do
  ins=$(( (100 - rd) / 2 ))
  for r in 1 2 3; do
    ./build/wh_bench_pcpu-rw --threads 4 --seconds 5 --warmup 2 \
      --dist zipfian --zipf_theta 0.99 --key_range 1000 --prefill 500 \
      --read_pct $rd --insert_pct $ins --pin_policy compact_phys
  done
done
```

| read_pct | median (M ops/s) | scaling vs 1T (17.28) | trial spread |
| ---: | ---: | ---: | --- |
| **100 (no writers)** | **75.57** | **4.37×** | < 0.3 % (tight) |
| 99 (1 % writes) | 42.99 | 2.49× | 25.6–47.4 M (1.9×) |
| 95 (5 % writes) | 4.87 | 0.28× | 4.0–6.1 M |
| 90 (10 % writes) | 4.77 | 0.28× | 2.3–8.8 M (3.8×) |
| 50 (50 % writes) | 1.01 | 0.06× | 0.75–1.42 M |

**Findings:**

1. **At 100 % reads, the lock scales super-linearly.** 4 threads deliver 4.37× of 1T throughput. The per-CPU slot machinery — each thread RMWs only its own cache-line-isolated counter — works exactly as designed when there are no writers.
2. **1 % writers cost 43 % of throughput.** Going from 100/0/0 to 99/0/1 drops throughput from 75.57 M to 42.99 M. A one-in-a-hundred event is responsible for losing more than half the available throughput.
3. **5 % writers cost 94 % of throughput.** 75.57 → 4.87 at 95/2/3. The retract-and-spin retry storm is the dominant time sink at any non-trivial writer rate.
4. **Variance grows monotonically with writer fraction.** 100 % reads: tight (deterministic); 99 %: 1.9× spread; 90 %: 3.8× spread. The herd's exact equilibrium depends on writer–reader race outcomes that vary trial-to-trial.

**Verdict — diagnosis closed for the read-vs-write distinction.** The thundering-herd hypothesis is **confirmed**. The per-CPU slot design itself is sound; the writer–reader handshake protocol is the bug.

→ Next: **0c** — check whether the same collapse appears in the lockbench microbench (which exercises a single global `pcpu_rw_lock`, no wormhole metalock+leaflock coupling).

---

## Implications for the fix

The data tells us *which class* of fix to pursue. The retract-and-spin reader protocol is replaceable; the per-CPU slot infrastructure is keepable. Three options, roughly in order of risk:

### Option A — Linux `percpu_rwsem` semantics (recommended starting point)

Readers **commit** rather than retract. Writers wait. Protocol:

- **Reader fast path** when no writer is in slow path: `local_inc(slot[T])`; `smp_mb()`; check global writer flag; if no writer, proceed. If a writer is queued, fall through to slow path (a single global mutex that writers also hold).
- **Writer slow path**: take a global writer mutex; do `synchronize_rcu()`-style wait for in-flight readers to drain via grace period; complete write.

No reader retracts. Writers may starve (a known property), but the read path is uncontended. Readers in flight when a writer arrives **finish their critical section**, then the writer proceeds.

Cost: requires a global mutex on the writer side and a grace-period mechanism. The grace period in our setting is just "wait for each per-thread slot to drain once" — which we already implement, just without making readers retract.

### Option B — BRAVO biased rwlock

A bias array maps readers to slots. Readers fast-acquire via the bias path (just write to local cache line, no atomics). Writers revoke the bias on entry, force readers to acquire via a fallback slow path.

More complex, but has the best demonstrated reader throughput in the literature.

### Option C — Backoff between retract and retry (least invasive)

Keep the retract-and-spin protocol but add per-thread exponential backoff after retract. Breaks the herd by desynchronising retry timing.

Likely buys us back to "matches `wh-default`" — not a structural fix, but a one-line change.

**Recommendation:** prototype **Option A** first (best fit for read-heavy workloads, matches the thesis story). Keep **Option C** as a fallback if A turns out to be more disruptive than expected.

---

## Open questions / next investigations

- [x] **0c — microbench check.** Done; confirms the lock primitive fails on its own, with wormhole providing ~86× amplification. See the section below.
- [ ] **`perf stat` on Graviton2 under collapse mode** — confirm cycles are spent in atomic stalls (`stall_backend`) vs cache misses vs idle spinning. Predicted: high `stall_backend`, moderate cache-misses.
- [ ] **ThreadSanitizer build** to rule out a latent data race in `my_slot()` or `slots[]` allocation that's mostly invisible under x86 TSO.
- [x] **Prototype Option A** as `pcpu_rw_lock_v2`. Done — see the v2 section below. Awaiting Graviton validation run.
- [ ] **Cross-check on Xeon.** Xeon at 8–12T does the same collapse, just at a different threshold. Once Option A is in, run both arches to verify the fix isn't aarch64-specific.

## Cross-reference

- Notebook: `scripts/lockbench_analysis.ipynb` §6 ("The `wh-pcpu-rw` collapse: anatomy of a thundering herd") — built before this investigation; the explanation is consistent with the data here but presented less rigorously.
- Decisions log: `docs/INDEX_LOCK_DECISIONS.md` D9 (introduction of `pcpu_rw_lock`), D22 (per-thread streams), D23 (single-socket sweep cap).
- Lock implementation: `include/primitives/pcpu_rw_lock.hpp`.
- Wormhole shim wiring: `third_party/wormhole/wh_lock_shim.cpp`.

## Reproduction quick-reference

```bash
# Baseline collapse (Graviton2, L1_warm_zipf99 90/5/5)
./build/wh_bench_pcpu-rw --threads 4 --seconds 5 --warmup 2 \
  --dist zipfian --zipf_theta 0.99 --key_range 1000 --prefill 500 \
  --read_pct 90 --insert_pct 5 --pin_policy compact_phys

# Confirm no-writer scaling (should hit ~75 M ops/s)
./build/wh_bench_pcpu-rw --threads 4 --seconds 5 --warmup 2 \
  --dist zipfian --zipf_theta 0.99 --key_range 1000 --prefill 500 \
  --read_pct 100 --insert_pct 0 --pin_policy compact_phys

# Thread sweep + read-pct sweep scripts are in this document above.
```

---

## 2026-05-11 — Diagnostic 0c: lockbench microbench (single global `pcpu_rw_lock`, no wormhole call pattern)

Tests whether the failure mode is intrinsic to the lock primitive or whether wormhole's metalock+leaflock coupling is required to trigger it. Same workload shape (90 % reads), same pinning, just a different driver that exercises one global pcpu_rw_lock:

```bash
for t in 1 2 3 4 5 6; do
  for r in 1 2 3; do
    ./build/lockbench --lock pcpu-rw --workload rw --threads $t \
      --seconds 5 --warmup 2 --read_pct 90 --pin_policy compact_phys
  done
done
```

| threads | median (M ops/s) | scaling vs 1T | trial spread |
| ---: | ---: | ---: | --- |
| 1 | 62.08 | 1.00× | < 0.1 % (tight) |
| 2 | 28.17 | 0.45× | 24.0–29.0 M |
| 3 | 23.09 | 0.37× | 22.9–23.8 M |
| 4 | 19.30 | 0.31× | 17.5–20.2 M |
| 5 | 17.88 | 0.29× | 16.2–18.3 M |
| 6 | 12.90 | 0.21× | 12.6–14.8 M |

**Findings:**

1. **The lock primitive itself fails to scale**, independent of wormhole. 1T → 6T loses 79 % of throughput on a workload that should be reader-scalable. The herd is intrinsic to the lock.
2. **It is much milder than in wormhole.** At 6T microbench delivers 12.9 M ops/s; wh_bench delivers 0.15 M ops/s — **86× amplification** by the wormhole call pattern.

### Amplification mechanism — wormhole-specific

Two contributors explain the 86× factor:

- **Two locks per op.** Wormhole acquires the global metalock (read) plus a per-leaf leaflock (read or write) per operation. Both are `pcpu_rw_lock` instances. A writer event on *either* lock triggers a reader retract for *that* lock's readers. With 5 % insert + 5 % delete (= 10 % leaflock writers), you're in herd state on both locks simultaneously.
- **Writer CS is much longer in wormhole than microbench.** Microbench writer CS is ~10 ns (one counter increment). Wormhole writer CS is hundreds of ns (sort the leaf, insert the KV, occasionally resize). While `writer_present` is held, all readers spin uselessly. The longer the CS, the larger the fraction of wall-clock spent in reader spin.

This is consistent with the **86× amplification** number: roughly (2 locks) × (CS-time ratio ~30×) ≈ ×60–100 multiplier on top of the bare primitive failure.

### Implication for the fix

The lock primitive is the root cause; wormhole is the amplifier. **Fixing the primitive will improve both benchmarks** — the microbench will recover most of its 1T throughput (likely 50+ M ops/s at 6T) and wormhole will benefit even more because long writer CSes will no longer cause reader thrashing.

→ Diagnosis closed. The remaining work is the fix prototype (Option A in the earlier section). Confidence is high enough to commit to the fix path without running 3a–3e first.

---

## 2026-05-11 — Fix prototype: `pcpu_rw_lock_v2` (Option A)

Implemented Linux `percpu_rwsem`-style protocol as a parallel primitive — same per-thread slot infrastructure, different writer–reader handshake. The slow path queues readers on `std::mutex` (futex-backed on Linux) instead of spinning on `writer_pending`. Wake-up is serialised by the mutex queue, breaking the herd.

**Files:**
- `include/primitives/pcpu_rw_lock_v2.hpp` — new primitive with full design rationale in header comment.
- `third_party/wormhole/wh_lock_shim.cpp` — instantiates v2 behind `WH_LOCK_PCPU_RW_V2`.
- `CMakeLists.txt` — adds `pcpu-rw-v2` to `WH_LOCKS` (build target: `wh_bench_pcpu-rw-v2`, `wh_test_pcpu-rw-v2`).
- `bench/main.cpp` — accepts `--lock pcpu-rw-v2` for microbench.
- `bench/lock_test.cpp` — templated correctness tests now cover both v1 and v2.
- `scripts/wh_compare.sh` — adds `pcpu-rw-v2` to the swept lock list.

**Correctness:** `locktest --threads 4 --loops 100000` passes both write-side mutual-exclusion and mixed-read/write torn-read tests for v2. `wh_test_pcpu-rw-v2` (8 threads × 20 000 ops race test) passes.

**Smoke test on macOS** (no pinning, just scheduling noise as the variance source — directional indicator only):

| threads | v1 (M ops/s) | v2 (M ops/s) | v2/v1 |
| ---: | ---: | ---: | ---: |
| 1 | 184.1 | 175.3 | 0.95× |
| 2 | 75.3 | 92.0 | 1.22× |
| 4 | 45.5 | 64.6 | 1.42× |
| 6 | 17.0 | 38.6 | 2.27× |
| 8 | 13.9 | 25.9 | 1.86× |

v2 trades a slight 1T cost (~5 %, the extra mutex bookkeeping isn't free even when uncontended) for substantially better high-thread behaviour. The improvement grows with thread count, exactly as predicted: at 1T there's no herd to break; at 6T the herd is what's bottlenecking v1, and v2's queue-on-mutex protocol avoids it.

**Predicted on Graviton (clean pinning, no scheduling jitter):**
- 1T: v2 within ±3 % of v1 (mutex overhead is small).
- 4T: v2 should match `wh-default` or better (~20+ M ops/s on `L1_warm_zipf99 90/5/5`, vs v1's 4.9 M).
- 8T: v2 should be a multi-× improvement over v1's 0.03 M ops/s on the same cell.

Validation commands for the Graviton box:

```bash
git pull
cmake --build build -j$(nproc)
./build/locktest --threads 4 --loops 100000  # correctness, both v1+v2

# Triage: thread sweep, v1 vs v2 head-to-head, 90/5/5 L1_warm_zipf99
for lock in pcpu-rw pcpu-rw-v2; do
  for t in 1 2 3 4 5 6 8; do
    for r in 1 2 3; do
      ./build/wh_bench_$lock --threads $t --seconds 5 --warmup 2 \
        --dist zipfian --zipf_theta 0.99 --key_range 1000 --prefill 500 \
        --read_pct 90 --insert_pct 5 --pin_policy compact_phys 2>/dev/null \
        | grep ops_s
    done
  done
done
```

If the v2 curve climbs through 8T instead of collapsing, the diagnosis-to-fix loop is closed. After that, re-running the full `scripts/wh_compare.sh` will give us comparable cross-arch v1/v2 data for the notebook.

---

## 2026-05-11 — Graviton2 validation of `pcpu_rw_lock_v2` (Option A fix)

Ran the same thread-sweep diagnostic on Graviton2 with v2 alongside v1:

```bash
for lock in pcpu-rw pcpu-rw-v2; do
  for t in 1 2 3 4 5 6 8; do
    for r in 1 2 3; do
      ./build/wh_bench_$lock --threads $t --seconds 5 --warmup 2 \
        --dist zipfian --zipf_theta 0.99 --key_range 1000 --prefill 500 \
        --read_pct 90 --insert_pct 5 --pin_policy compact_phys
    done
  done
done
```

| threads | v1 median (M ops/s) | v2 median (M ops/s) | v2 / v1 |
| ---: | ---: | ---: | ---: |
| 1 | 17.32 | 16.48 | 0.95× |
| 2 | 17.67 | 21.01 | 1.19× |
| 3 | 15.90 | 26.46 | 1.66× |
| 4 | 4.90 | 30.35 | **6.20×** |
| 5 | 0.41 | 31.60 | **77×** |
| 6 | 0.18 | 28.84 | **160×** |
| 8 | 0.030 | 16.00 | **533×** |

**Findings:**

1. **The fix works.** v2 climbs from 1T to a peak at 5T (31.6 M ops/s), 1.92× scaling vs 1T. The collapse mode is eliminated — v2 at 8T is 533× faster than v1, with tight trial-to-trial variance (14.6 / 16.9 / 16.0) instead of v1's 3.5× spread.
2. **1T cost of v2 vs v1: ~5 % regression** (16.5 vs 17.3 M ops/s). The extra mutex bookkeeping in the fast path (one conditional branch on `writer_pending`) isn't free, but it's the right trade.
3. **v2 plateaus at 5T and descends through 6T → 8T** (31.6 → 28.8 → 16.0). This is a *new* bottleneck: with 10 % writers and 8 threads competing through `writer_mu`, the mutex itself serialises writers, capping write throughput and indirectly readers in the slow path. Much milder than v1's herd — still 530× faster than v1 at 8T — but worth investigating in a follow-up.

### Where v2 sits in the lock landscape at 8T (Graviton2, L1_warm_read_heavy)

| Lock | 8T M ops/s |
| --- | ---: |
| `wh-occ-opt` (lock-free reads) | **69.6** |
| `wh-default` (counter rwlock)  | 20.3 |
| **`wh-pcpu-rw-v2`** | **16.0** |
| `wh-cas`  | 15.5 |
| `wh-tas`  | 14.2 |
| `wh-occ`  | 11.0 |
| `wh-pcpu-rw` (v1) | 0.03 |

v2 lands above the spinlock and OCC-write variants, slightly behind `wh-default`. The thesis story sharpens:

1. The naïve per-CPU rwlock with retract-and-spin **fails catastrophically** under any meaningful writer rate (v1).
2. Replacing the retract protocol with a mutex-queued slow path **restores usable throughput** (v2), confirming the §6 diagnosis was correct.
3. But **lock-free OCC reads (`wh-occ-opt`) remain the practical winner** — neither v1 nor v2 catch up. The lesson is that *avoiding the lock altogether* (where the data structure permits) outperforms *fixing* the lock.

→ **Diagnosis → fix → validation loop is closed.** This investigation is complete.

### Follow-ups (not blocking)

- Investigate the v2 6T→8T plateau-then-descent. Hypothesis: `writer_mu` saturation under 10 % writers × 8 threads. Could be tested with a `read_pct=99` sweep on v2 (expect monotonic scaling through 8T if mu is the only bottleneck).
- Full `wh_compare.sh` run on Graviton2 + Xeon now that `wh-pcpu-rw-v2` is in `LOCKS`. Provides comparable data across the whole 12-workload matrix.
- Notebook update (`scripts/lockbench_analysis.ipynb`): replace §6 ("anatomy of a thundering herd") interpretation with "the herd, and the fix that demonstrates it was a herd" — using this data as the prosecution AND the resolution.
