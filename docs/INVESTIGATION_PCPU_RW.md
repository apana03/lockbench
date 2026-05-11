# Investigation — `pcpu_rw_lock` collapse on Graviton2

**Status:** diagnosis confirmed; fix design pending implementation. ↗ See also `docs/INDEX_LOCK_DECISIONS.md` D9 (introduction of the primitive) and the post-implementation findings appended to D22.

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

- [ ] **0c — microbench check.** Does `./build/lockbench --lock pcpu-rw --workload rw` show the same collapse curve? If yes, the failure is entirely intrinsic to the lock primitive; if no, wormhole's metalock+leaflock coupling amplifies the herd and the fix may have a workload component.
- [ ] **`perf stat` on Graviton2 under collapse mode** — confirm cycles are spent in atomic stalls (`stall_backend`) vs cache misses vs idle spinning. Predicted: high `stall_backend`, moderate cache-misses.
- [ ] **ThreadSanitizer build** to rule out a latent data race in `my_slot()` or `slots[]` allocation that's mostly invisible under x86 TSO.
- [ ] **Prototype Option A** as `pcpu_rw_lock_v2` (parallel primitive in `include/primitives/`). Wire into the wormhole shim as a separate `wh-pcpu-rw-v2` build, sweep the same matrix, compare directly.
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
