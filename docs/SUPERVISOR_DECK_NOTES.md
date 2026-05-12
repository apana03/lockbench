# Speaker notes — `2026-05-11_cross_arch_findings.pptx`

Companion to the deck for the 2026-05-11 supervisor meeting. One section per slide; talking points, expected questions, and where to drill in if asked.

## 1. Title

"Lock primitives across x86_64 and aarch64." Open with: "I want to walk you through the cross-platform findings — but first let me give you a 5-minute status update on what's changed since we last met, because one of the locks behaved very differently than the literature predicted."

Then verbal update from `docs/SUPERVISOR_UPDATE_2026-05-11.md` (~5 min). Resume the deck at slide 2.

## 2. What we set out to answer

Frame the two questions explicitly:

- **Practical:** which lock should you pick for a concurrent ordered index? Does the answer change on ARM vs x86?
- **Scientific:** if it changes, *which* microarchitectural feature is responsible?

The architecture differs in three orthogonal axes: ISA atomics, clock + DRAM, and memory model. Each predicts different lock effects. The benchmark is designed to separate them.

Sub-question worth flagging: "we expected the per-CPU rwlock to be the winner. It isn't. The reason is the most interesting result in the deck."

## 3. Experimental setup

Three things to emphasise:

- **Same harness, three indexes.** Wormhole is the rwlock workhorse; StripedMap/AVL are exclusive-lock-only, so they're the cross-bench robustness check for the spinlock ranking. The rwlock story lives entirely in wormhole.
- **Same workload matrix on both archs.** 12 cells × 5-point ladder × 3 trials. ~6.6 h per arch.
- **Topology-aware ladder.** Each x-tick is a real architectural transition (1–2 within core pair, 4 within L2 group, 8/12 fills socket 0). Not arbitrary powers of two.

If asked about methodology depth: refer to D1–D23 in the decisions log.

## 4. Architectural reference (table)

This slide is the rosetta stone for everything that follows. Memorise the three predicted ratios from spec sheets:

- **1.39× from clock** (2.5 / 1.8) — Graviton's tailwind if the workload is CPU-bound.
- **1.50× from DRAM** (DDR4-3200 / DDR4-2133 bandwidth) — tailwind if memory-bound.
- **~2× from LSE atomics** — tailwind if atomic-bound.

Every cross-arch ratio in the deck refers back to these three. If a lock's ratio is bigger than 1.39× but less than 1.50×, we know it's partially clock-bound and partially DRAM-bound. If it exceeds 1.50× it's drawing on the LSE win.

## 5. Finding 1 — 1T baseline (bar chart)

Slide message: **uncontended, the architectural gap is small and tight** (1.20–1.34×).

Walk-through:

- All bars are in 50–82 ns/op range. Critical sections are short.
- Graviton consistently faster but by less than the 1.39× clock prediction. Why? Memory-stall on the leaf walk doesn't shrink linearly with clock.
- The ranking *within* each arch is identical: tas ≈ ttas ≈ cas ≈ occ slightly above pcpu-rw and default. Lock cost order is microarchitecture-independent.

Expected question: "So why is wh-default the slowest single-threaded?" Answer: its fast path is a CAS (LOCK CMPXCHG / CASAL), heavier than a single XCHG. The CAS-vs-XCHG cycle gap matters most at 1T when there's nothing else to amortise it against.

## 6. Reading the 1T baseline (bullets)

This slide formalises the previous chart. The key insight: "the 1T ratio gives us a budget. Any cross-arch speedup beyond ~1.34× at higher thread counts has to be attributed to something the architecture is doing for parallelism — coherence cost, atomic latency under contention, scheduler interaction." That sets up the next two slides.

## 7. Finding 2 — Scaling grid (figure)

Slide message: **on the headline workload, wh-occ-opt dominates everywhere; wh-pcpu-rw is the anomaly.**

Walk-through:

- Each row = one workload, same y-axis on the two arches in that row, so you can read the cross-arch gap visually.
- L1_warm (top): every lock scales reasonably except v1 pcpu-rw on Graviton (the green line that drops out of the Graviton panel).
- L1_extreme (middle): even wh-occ-opt curves flatten — when all readers fight for the same few keys, lock-free doesn't help, the keys do.
- L3_warm (bottom): wh-occ-opt absolutely takes off. This is where DRAM bandwidth matters and Graviton's advantage stretches.

Expected question: "Why does the Graviton ladder stop at 8?" — that's the physical core count of the AWS c6g instance we have. Future work could push to 16 on c6g.16xlarge.

## 8. What the scaling grid shows (bullets)

Three things to say verbally:

- "On read-heavy, you should be using a lock-free read path if you can."
- "Spinlocks overlap because at this critical section length, line-bouncing dominates instruction cost."
- "The cross-arch *ranking* is preserved on every panel. The *gap* shifts."

## 9. Finding 3 — Cross-arch ratio (figure)

Slide message: **most locks earn the clock-speed bonus (~1.4×) plus a bit more from LSE atomics. One lock inverts it.**

Walk-through:

- All the well-behaved locks sit just above the green dotted line (1.39× clock prediction) — coherent with theory.
- spinlock lines stack on top of each other — atomic type irrelevant at high contention.
- v1 pcpu-rw plunges through parity at ~3 threads and falls to 1% of Xeon at 8T. **This is the most striking result.**

Expected question: "Why is Graviton *worse*? Aren't faster atomics universally good?" Answer is the segue to the collapse story: it depends what you're doing with them. Fast atomics let readers retry faster, which makes the herd tighter. Save the detailed explanation for slide 12.

## 10. Microarchitectural decomposition (bullets)

Quantitative attribution per lock:

- Spinlocks: 2–3× → atomic-bound, LSE wins
- wh-default: 1.5–2× → coherence-bound, both arches converge
- wh-occ-opt: 1.3–1.6× → memory-bound (DRAM helps); doesn't touch lock state
- pcpu-rw: inverts → herd is timing-sensitive to atomic speed

Use this slide to make the point: "we can read the cross-arch ratio as a fingerprint of *what* the lock is bottlenecked on. That fingerprint identifies the design class."

## 11. Heatmap (figure)

Slide message: **the pattern from the headline workload generalises.** Most cells green; pcpu-rw row red.

Walk-through:

- Greenest cell = wh-occ-opt + L3_warm, the prediction from the bandwidth-advantage story.
- Reddest non-pcpu cells = L1_extreme — when hot-key contention dominates, both arches do the same amount of serialised work.
- The pcpu row breaks the heatmap: anywhere there are writers, it's red.

## 12. Heatmap readings (bullets)

Reinforce: green = lock is bottlenecked on something Graviton handles better. Red = Xeon handles it better OR the lock is broken on Graviton.

## 13. (Section header) "The pcpu_rw_lock story — diagnose → fix → validate"

Verbal transition: "This investigation is the most rigorous thing I did this month. The notebook flagged the anomaly; I diagnosed it with two targeted sweeps; the diagnosis predicted a fix; the fix works. Walking through it in four slides."

## 14. Finding 4 — naïve per-CPU rwlock fails

Establish the surprise:

- Was supposed to be the headline win — per-CPU counters on isolated cache lines, no coherence traffic on reads.
- 1T performance matches the design (17 M ops/s, parity with wh-default).
- 8T: 0.03 M ops/s. **600× collapse.**
- Three trials at 8T: 38 K, 32 K, 11 K — *non-deterministic*, so it's not just slow, it's a phase-transition behaviour.

Expected question: "Could this be a bug?" — yes I thought so, ran the microbench standalone (no wormhole), still fails. The primitive is the root cause; wormhole amplifies ~86× via two locks/op + longer writer CS.

## 15. Read-pct sweep (figure)

Slide message: **the writers are the trigger.** This is the clean experiment.

- 100/0/0: super-linear scaling, 4.37× at 4T. Per-CPU machinery works perfectly when writers are absent.
- 1 % writers: lose 43 %.
- 5 % writers: lose 94 %.
- 50 % writers: lose 99 %.

Verbal: "A 1-in-100 writer event costs more than half the throughput. That's not 'writer is slow.' That's 'every writer arrival triggers a system-wide event.'"

## 16. Diagnosis: thundering herd (bullets)

Walk through the mechanism step by step:

1. Reader fast path = fetch_add(slot.count); check writer_present; if set, fetch_sub; spin.
2. Writer arrives, sets writer_present.
3. All concurrent readers see it, all retract, all spin.
4. Writer scans 64 slot counters in a loop. As readers retract, the scan sees 0s.
5. Writer releases. All readers stampede back. Goto 2.

Key insight (this is the part that surprises people): **on Graviton, faster atomics make this worse.** The readers' retry loop is tighter, so they all complete fetch_add within nanoseconds of each other, giving the writer a full house of 8 slots to drain every cycle. On Xeon's slower atomics, the readers spread out a bit, accidentally throttling the herd. Slow atomics save you from a buggy protocol.

Cross-reference: `docs/INVESTIGATION_PCPU_RW.md` has the full diagnostic chain (thread sweep, read-pct sweep, microbench standalone, amplification analysis).

## 17. Fix: pcpu_rw_lock_v2

The fix is to **change the handshake, not the data layout.** The per-thread slot array was right. The protocol was wrong.

Linux's `percpu_rwsem` semantics:

- Readers **commit and proceed.** No retraction.
- Writers acquire a mutex on the slow path, then drain readers without forcing them off.
- A reader caught mid-acquire by an arriving writer **finishes its critical section**.

Cost: ~5 % at 1T (one extra branch on the fast path). Trade is obviously correct.

Correctness validated by locktest (race tests, 8 threads × 20 000 ops, both write-mutex and torn-read).

## 18. Finding 5 — v2 eliminates the collapse (figure)

Slide message: **same workload, same machine, same code path, completely different curve.**

- v1 (green) falls off the chart.
- v2 (cyan) climbs to a peak at 5T (~32 M/s), then declines gently to 16 M/s at 8T.
- For reference: wh-default (gray dashed) plateaus around 20 M/s; wh-occ-opt (brown dashed) keeps climbing to 70 M/s.

Verbal: "The v2 6T→8T decline is a *new* bottleneck — writer-side mutex saturation under 10% writers × 8 threads. That's a follow-up. But it's a manageable bottleneck, not a herd catastrophe."

## 19. v1 vs v2 table

Numbers to point at:

- 4T: 4.9 → 30.4 M/s = **6.2×**
- 8T: 0.03 → 16.0 M/s = **533×**

The 533× is the headline. If the supervisor only remembers one number from the meeting, this is it.

## 20. Final ranking table at 8T Graviton

Use this to summarise the lock landscape:

- **occ-opt at 69.6** = clear winner if your data structure supports it
- **default at 20.3** = robust mid-tier
- **v2 at 16.0** = the fixed per-CPU rwlock; lives in the middle
- **spinlocks at 14–15** = the old reliable
- **occ at 11.0** = seqlock writes still pay CAS overhead
- **v1 at 0.03** = the lesson

## 21. Five findings (recap)

This is the takeaway slide. Read each one slowly:

1. Lock-free OCC reads win categorically — 3.4–3.7× over counter rwlocks.
2. Counter rwlocks are robust — don't scale linearly but never collapse.
3. Naïve per-CPU rwlocks aren't a drop-in fix; protocol matters.
4. Graviton2 is 1.4–2.8× faster than Xeon on every well-behaved lock — predictable from spec ratios.
5. Faster atomics can make a fragile lock fail harder. Hardware modernisation can expose latent timing bugs.

Finding 5 is the punch line. Hardware that *should* make everything better in fact reveals a class of bugs that were dormant on slower hardware.

## 22. Why I trust these numbers

Use this slide if asked "how confident are you in the variance bands?":

- compact_phys pinning (no SMT confounds, no cross-socket)
- main-thread prefill pinning (D16) prevents first-touch on wrong socket
- topology-aware ladder maps inflections to architecture
- pre-rolled streams (D22) take RNG out of the timed window
- single-socket cap removes NUMA confounds
- microbench cross-check on the pcpu primitive standalone
- locktest validates v1 and v2 correctness

CoV is < 5 % on most cells. The high-CoV cells are pcpu-rw v1 specifically — that's the failure mode, not measurement noise.

## 23. Open questions

Three to raise:

1. **NUMA story** — currently dropped (D23). Worth ~3.3 h to recover. Up to supervisor whether the cross-socket coherence cost belongs in the thesis.
2. **v2 full sweep** — currently only diagnostic-cell data. The full 12-workload sweep across both arches with v2 in the lock list is queued.
3. **Notebook §6 needs updating** — currently still reads as "anatomy of a thundering herd" without showing the v2 resolution. The deck has the full arc, but the notebook is the long-form artifact.

## 24. Next steps

If signed off this week:

- Rerun `wh_compare.sh` with v2 in the lock list (~6.6 h × 2 arches).
- Update notebook §6 with the v2 data.
- Draft thesis chapter using deck structure as the spine.

If the supervisor pushes for the NUMA story too: another ~3.3 h on Xeon, results pipelined into the notebook §12+.

---

## Likely awkward questions and how to handle them

- **"Why didn't you predict the collapse from the protocol design?"** Honest answer: I didn't model the timing relationship between reader retry latency and writer arrival rate before running it. The literature on per-CPU rwlocks (PERCPU-RWSEM in the Linux kernel) describes the *fix* but not the failure mode as a function of atomic speed. The fact that Graviton makes it worse than Xeon was genuinely surprising and is the most novel finding.

- **"Is this finding generalisable beyond this microarchitecture pair?"** It should be — any pair of architectures where one has substantially faster RMW atomics than the other will show the same effect on this protocol. The directional claim (faster atomics → worse herd) is architecture-independent. The magnitude depends on the writer CS length.

- **"Why didn't you just use Linux percpu_rwsem from the start?"** I wanted to test the *design class* of "per-CPU rwlock" with a simple, transparent implementation. The naïve retract-and-spin version is in plenty of textbook treatments of per-CPU rwlocks. Discovering that the textbook version is broken on modern hardware is a more interesting finding than just confirming that Linux's mature implementation works.

- **"Is StripedMap and BronsonAVL data wasted effort?"** No, but it's a robustness check, not the main result. The spinlock ranking holds across all three data structures on both arches — that lets us say with confidence that the lock-vs-lock differences we see in wormhole are about lock behaviour, not wormhole-specific artifacts.

- **"How does this compare to existing literature?"** Calciu et al. (PPoPP 2013) is the canonical paper on the cache-coherence cost of counter-based rwlocks; we confirm their model empirically on both arches. The percpu_rwsem-style fix mirrors Linux kernel design. The cross-arch failure-mode finding (Finding 5) is, as best I can tell, novel.
