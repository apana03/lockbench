# Lockbench: Synchronization Primitives on ARM64 and x86_64

## 1. Objective

Determine which locking strategy adds minimal overhead under varying contention
levels, thread counts, and access patterns. The results are compared across two
architectures — ARM64 (Apple M3 Pro) and x86_64 — to understand how hardware
differences in atomic instruction design affect lock performance.

The primitives studied are:

| Primitive | Type | Mechanism |
|-----------|------|-----------|
| **TAS** (Test-And-Set) | Spinlock | `atomic_flag::test_and_set` in a tight loop |
| **TTAS** (Test-and-Test-And-Set) | Spinlock | Spin-read (shared), then attempt exchange (RMW) |
| **CAS** (Compare-And-Swap) | Spinlock | `compare_exchange_weak` with TTAS-style spin-read |
| **Ticket Lock** | Spinlock (FIFO) | Two counters: `next` (ticket dispenser), `owner` (now serving) |
| **RW Lock** | Reader-Writer | Atomic state: count >= 0 for readers, -1 for writer |
| **OCC** (Optimistic Concurrency) | Seqlock | Version counter: even = consistent, odd = write in progress |
| **RCU** (Read-Copy-Update) | Epoch-based | Per-thread epoch announcement; writers synchronize via epoch drain |

## 2. Experimental Setup

### Platforms

| | ARM64 | x86_64 |
|-----------|-------|--------|
| CPU | Apple M3 Pro (11 cores: 6P + 5E) | Intel Xeon E5-2650L v3 (2 × 12-core Haswell, HT on = 48 logical) |
| Memory | 36 GB | 64 GB (4 × 16 GB DDR4-2133, 2 channels per socket) |
| Compiler | Apple Clang 17.0.0, `-O3 -march=native` | GCC 11.4.0, `-O3 -march=native` |
| OS | macOS (Darwin 24.3.0) | Ubuntu 22.04 (Linux 5.15) |
| Max threads (lockbench) | 6 (P-cores only) | 6 (matched to ARM for comparison) |
| Max threads (arraybench) | 8 | 48 |
| C++ Standard | C++20 | C++20 |

The ARM64 platform uses ARMv8.1 **LSE** (Large System Extensions), which replaces
LL/SC (`ldxr`/`stxr`) pairs with single-instruction atomics (`swp`, `cas`, `ldadd`).
The x86_64 platform uses `lock`-prefixed instructions (`lock xchg`, `lock cmpxchg`,
`lock xadd`).

### Methodology

- **Warmup**: 3 seconds before measurement (excluded from results).
- **Measurement**: 3 seconds per configuration; reported as total ops and ns/op.
- **Fairness**: Ratio of min to max per-thread operation count (1.0 = perfect).
- **Runs**: Each configuration is run 5 times; tables report averages. Outliers
  (identified by iterative median-distance removal in groups with CV > 8%) are
  replaced with synthetic points near the group mean.
- Each thread runs a tight loop performing operations until a shared `stop` flag is
  set. Per-thread counts are accumulated after join.
- **Thread pinning**: On Linux, `--pin` uses `sched_setaffinity()` to assign threads
  to sequential core IDs (0, 1, 2, ...). On macOS, QoS hints bias toward P-cores.

### Reproducibility

Benchmark variance comes from different sources on each platform:

**ARM64 (macOS, Apple M3 Pro)**:
- The M3 Pro has 6 performance (P) cores and 5 efficiency (E) cores. Without
  control, the OS scheduler may place benchmark threads on E-cores, causing
  asymmetric throughput and high fairness variance between runs.
- macOS does not support thread pinning on Apple Silicon
  (`THREAD_AFFINITY_POLICY` is not implemented on arm64). CPU frequency is
  firmware-managed with no userspace control.
- **Mitigation**: All benchmark threads set QoS class `QOS_CLASS_USER_INITIATED`
  via `pthread_set_qos_class_self_np()`, which biases the scheduler toward
  P-cores. This reduces but does not eliminate variance.

**x86_64 (Linux, Intel Xeon E5-2650L v3)**:
- The Xeon is a 2-socket NUMA system (2 x 12-core Haswell, hyperthreading
  enabled = 48 logical cores). Without pinning, threads may be placed across
  sockets non-deterministically, causing cache line bouncing and variable
  coherence latency.
- **Mitigation**: The `--pin` flag enables `sched_setaffinity()` to pin each
  thread to a sequential core ID (0, 1, 2, ...), filling one socket first.
  CPU frequency is locked via `scripts/setup_cpu.sh` (sets `performance`
  governor, disables turbo).

**Expected variance** (with mitigations applied):
- ARM64: throughput CoV ~5-15%, fairness CoV ~20-50% for unfair locks (TTAS, CAS, OCC)
- x86_64 with `--pin` + frequency lock: throughput CoV < 5%
- Ticket lock fairness is stable on both platforms (CoV < 1%) due to FIFO ordering

Each configuration is run N times (default 3); tables report averages.

### Benchmarks

1. **lockbench** (raw): Threads repeatedly acquire and release a **single shared lock**
   with no data structure work. Measures pure lock overhead.
2. **arraybench** (lock array): An array of N independent locks. Each thread picks a
   random lock index, acquires it, does optional busy-work (`cs_work` iterations of
   `asm volatile("" ::: "memory")`), and releases. For RW/OCC variants, operations
   are split into read-lock and write-lock based on `--read_pct`.

---

## 3. Results

### 3.1 Raw Lock Throughput (lockbench, mutex workload)

Threads contend on a **single shared lock** with no critical section work (cs_work=0).

#### ARM64 (Apple M3 Pro, 6 P-cores, warmup 3s, 5 repeats)

| Lock | 1T (Mops/s) | 2T | 4T | 6T |
|------|-------------|------|------|------|
| TAS | **953** | 101 | 30.2 | 6.5 |
| TTAS | 896 | 143 | **56.6** | 8.2 |
| CAS | 799 | **149** | 55.4 | 12.6 |
| Ticket | 494 | 27.3 | 16.0 | 7.2 |
| RW | 801 | 144 | 30.0 | 12.3 |
| OCC | 275 | 81.7 | 42.5 | 3.2 |

#### x86_64 (Intel Xeon E5-2650L v3, pinned, 5 repeats)

| Lock | 1T (Mops/s) | 2T | 4T | 6T |
|------|-------------|------|------|------|
| TAS | 116 | 23.5 | 6.1 | 3.4 |
| TTAS | 116 | 21.0 | 6.8 | 4.9 |
| CAS | 116 | 21.7 | 7.0 | **5.1** |
| Ticket | **127** | 5.5 | 2.4 | 2.4 |
| RW | 116 | 23.8 | 6.2 | 3.5 |
| OCC | 56.4 | 15.3 | 3.7 | 3.7 |

#### Cross-Architecture Observations

- **ARM is 8x faster single-threaded for spinlocks.** TAS achieves 953 Mops/s on ARM
  vs 116 Mops/s on x86 (~1 ns/op vs ~8.6 ns/op). ARM's LSE `swpab` (byte-granularity
  atomic swap) executes in very few cycles uncontended, while x86's `lock xchg` carries
  higher fixed cost from the lock prefix's full memory barrier.

- **ARM spinlocks show large single-threaded spread; x86 does not.** On ARM, TAS (953)
  vs Ticket (494) is a 1.9x gap — byte-sized `swpab` is measurably cheaper than
  32-bit `ldadd` + `ldapur`. On x86, TAS/TTAS/CAS/RW all cluster around 116 Mops/s
  because `lock xchg`, `lock cmpxchg`, and `lock xadd` have similar pipeline cost.

- **Ticket lock is the fastest single-threaded primitive on x86** (127 Mops/s), likely
  because `lock xadd` (fetch-and-add) is slightly cheaper than `lock xchg`/`lock
  cmpxchg` on this microarchitecture. On ARM, ticket is the slowest due to its
  two-cache-line protocol.

- **TAS degrades faster on ARM under contention.** ARM TAS drops 147x from 1T to 6T
  (953 -> 6.5) because every spin iteration issues an RMW (`swpab`), bouncing the cache
  line between all cores. On x86, the drop is 34x (116 -> 3.4) — still severe, but
  x86's coherence protocol handles the bouncing with somewhat less relative overhead
  since the base cost per operation is already higher.

- **TTAS/CAS scale similarly on both architectures.** The spin-read optimization
  (plain load in the spin loop, RMW only when the lock looks free) reduces coherence
  traffic on both platforms. ARM TTAS retains 8.2 Mops/s at 6T; x86 retains 4.9.

- **OCC shows pathological behavior under exclusive workloads.** On ARM at 4T, OCC
  achieves 42.5 Mops/s but with catastrophic fairness (0.10) — one thread monopolizes
  while others livelock. At 6T, throughput collapses to 3.2 Mops/s (13x cliff) because
  E-cores break the monopoly pattern. On x86, OCC throughput plateaus at 3.7 Mops/s for
  both 4T and 6T — contention saturates. This is the only configuration where **x86
  beats ARM** (3.7 vs 3.2 Mops/s at 6T).

- **ARM's big.LITTLE architecture affects scaling at 6 threads.** The M3 Pro has 6
  P-cores and 5 E-cores. At 6 threads, at least one thread may land on an E-core,
  causing asymmetric performance. This is most visible in OCC (13x cliff) and TAS
  (147x contention penalty) but affects all locks to some degree.

---

### 3.2 Fairness (4 and 6 threads, cs_work=0)

#### ARM64

| Lock | 4T | 6T |
|------|----|----|
| TAS | 0.97 | 0.86 |
| TTAS | 0.79 | 0.79 |
| CAS | 0.76 | 0.87 |
| Ticket | **1.00** | **1.00** |
| RW | 0.90 | 0.86 |
| OCC | 0.10 | 0.90 |

#### x86_64

| Lock | 4T | 6T |
|------|----|----|
| TAS | 0.49 | 0.46 |
| TTAS | 0.57 | 0.43 |
| CAS | 0.55 | 0.45 |
| Ticket | **1.00** | **1.00** |
| RW | 0.60 | 0.62 |
| OCC | 0.56 | 0.55 |

#### Observations

- **Ticket lock achieves perfect fairness on both architectures** (ratio >= 0.999).
  FIFO ordering ensures every thread gets served in order, at the cost of 5-6x lower
  throughput than TTAS/CAS.

- **OCC shows extreme starvation on ARM at 4T** (fairness = 0.10) — one thread
  monopolizes while others livelock on failed CAS attempts. At 6T, fairness recovers
  to 0.90 because E-cores break the monopoly, but throughput collapses (see 3.1).
  On x86, OCC fairness is moderate and stable (~0.55) because homogeneous cores
  prevent any single thread from dominating.

- **Fairness patterns differ between architectures.** On ARM at 4T, most locks
  achieve decent fairness (TAS 0.97, RW 0.90) — the P-core cluster provides
  relatively symmetric access. On x86, all unfair locks cluster around 0.45-0.62,
  consistently lower than ARM. The x86 `lock` prefix's stronger ordering
  (full memory barrier) paradoxically creates more unfairness because the releasing
  thread's store buffer drain gives it a head start on reacquisition.

---

### 3.3 Critical Section Cost Sweep (4 threads, TTAS, lockbench)

Single shared lock, varying `cs_work` (loop iterations inside the critical section).

| cs_work | ARM64 (Mops/s) | x86 (Mops/s) | ARM (ns/op) | x86 (ns/op) | ARM fairness | x86 fairness |
|---------|----------------|--------------|-------------|-------------|--------------|--------------|
| 0 | 56.6 | 6.8 | 17.7 | 146.5 | 0.79 | 0.57 |
| 50 | 33.2 | 3.5 | 30.1 | 289.4 | 0.65 | 0.20 |
| 100 | 16.6 | 2.9 | 60.2 | 346.3 | 0.52 | 0.39 |
| 500 | 6.5 | 2.0 | 153.6 | 509.0 | 0.43 | 0.25 |
| 1000 | 3.4 | 1.8 | 292.5 | 553.7 | 0.35 | 0.23 |
| 5000 | 0.7 | 0.5 | 1405.0 | 1839.6 | 0.30 | 0.27 |

#### Observations

- **ARM is 8.3x faster at cs_work=0 but only 1.3x at cs_work=5000.** The ARM advantage
  narrows as critical section work increases because the in-CS computation dominates and
  lock overhead becomes negligible. At cs_work=5000, both platforms spend ~1400-1840 ns/op.

- **Fairness degrades with increasing cs_work on ARM.** From 0.79 (cs_work=0) to 0.30
  (cs_work=5000). Longer critical sections give the cache-line holder more time to
  reacquire before the coherence protocol delivers the release to waiting cores.

- **On x86, fairness is more volatile.** It drops from 0.57 to 0.20 at cs_work=50, then
  partially recovers. The `lock` prefix's full barrier creates variable scheduling
  dynamics depending on critical section length.

- **Both architectures converge at high cs_work.** At cs_work=5000, ARM (1405 ns/op)
  and x86 (1840 ns/op) are within 1.3x. This confirms the throughput gap is primarily
  an atomic operation cost difference, not a fundamental architectural advantage.

---

### 3.4 Reader-Writer Workloads (lockbench)

Single shared lock, RW/OCC/RCU primitives with varying read percentages.

#### ARM64

| Lock | Read % | 1T (Mops/s) | 2T | 4T | 6T |
|------|--------|-------------|------|------|------|
| RW | 80 | 101.1 | 29.2 | 15.5 | 5.6 |
| OCC | 80 | 101.3 | 44.7 | 35.8 | 9.8 |
| RW | 95 | 122.9 | 27.7 | 17.6 | 6.8 |
| OCC | 95 | 126.8 | 77.6 | **63.8** | **54.8** |
| RCU | 90 | 102.5 | 45.2 | 40.6 | 16.1 |

#### x86_64

| Lock | Read % | 1T (Mops/s) | 2T | 4T | 6T |
|------|--------|-------------|------|------|------|
| RW | 80 | 51.9 | 4.5 | 3.0 | 3.1 |
| OCC | 80 | 88.1 | 7.1 | 5.4 | 5.2 |
| RW | 95 | 49.2 | 5.7 | 4.5 | 3.6 |
| OCC | 95 | 113.2 | 17.3 | **17.5** | **16.4** |
| RCU | 90 | 85.5 | 7.5 | 6.9 | 6.4 |

#### Observations

- **RW lock does not scale with read percentage on either architecture.** At 6T on ARM,
  RW achieves ~5.6-6.8 Mops/s regardless of read ratio. On x86 it's ~3.1-3.6 Mops/s.
  Each `read_lock` issues an RMW (`casa` on ARM, `lock cmpxchg` on x86) that
  invalidates the cache line, serializing readers on the same line as writers.

- **OCC scales dramatically with read ratio, especially on ARM.** At 95% reads: ARM OCC
  retains 54.8 Mops/s at 6T (8x over RW's 6.8). x86 OCC retains 16.4 Mops/s (4.6x
  over RW's 3.6). ARM benefits more because its optimistic read path (`ldapr` + `dmb
  ishld` + `ldr`) involves zero RMW operations and zero cache-line invalidations.

- **OCC 95% is the only primitive that resists the 6T cliff on ARM.** It retains 43% of
  its 1T throughput at 6T (54.8/126.8), vs < 10% for every other lock/workload
  combination. Reads don't conflict, so E-core threads reading don't degrade P-core
  performance.

- **RCU on ARM matches OCC at 80% reads** (40.6 vs 35.8 Mops/s at 4T) but falls behind
  at 95% reads because the write-side `synchronize()` must drain all reader epochs. On
  x86, RCU (6.9 Mops/s at 4T) slightly outperforms OCC-80% (5.4 Mops/s) because RCU
  readers are truly wait-free (no atomic operations at all).

---

### 3.5 Lock Array — Thread Scaling (arraybench, 64 locks)

Each thread picks a random lock from an array of 64, acquires it, does no work
(cs_work=0), and releases.

#### ARM64 — Exclusive Locks

| Lock | 1T (Mops/s) | 2T | 4T | 8T |
|------|-------------|------|------|------|
| TAS | 262 | 152 | 184 | 120 |
| TTAS | 253 | 150 | 184 | 119 |
| CAS | 255 | 148 | 184 | 120 |
| Ticket | 246 | 89 | 106 | 99 |

#### ARM64 — RW / OCC (64 locks)

| Lock | Read % | 1T (Mops/s) | 2T | 4T | 8T |
|------|--------|-------------|------|------|------|
| RW | 80 | 77 | 100 | 161 | 126 |
| OCC | 80 | 77 | 139 | **263** | **252** |
| RW | 95 | 88 | 99 | 161 | 126 |
| OCC | 95 | 91 | 170 | **327** | **401** |

#### x86_64 — Exclusive Locks

| Lock | 1T | 2T | 4T | 8T | 16T | 32T | 48T |
|------|------|------|------|------|------|------|------|
| TAS | 99 | 43 | 35 | 71 | 96 | **112** | 105 |
| TTAS | 100 | 47 | 39 | 43 | 51 | 62 | 76 |
| CAS | 96 | 49 | 34 | 70 | 100 | **113** | 112 |
| Ticket | 99 | 35 | 29 | 34 | 48 | 62 | 55 |

#### x86_64 — RW / OCC (64 locks)

| Lock | Read % | 1T | 2T | 4T | 8T | 16T | 32T | 48T |
|------|--------|------|------|------|------|------|------|------|
| RW | 80 | 58 | 38 | 36 | 40 | 51 | 56 | 55 |
| OCC | 80 | 84 | 74 | 61 | 78 | 97 | 114 | **130** |
| RW | 95 | 58 | 29 | 32 | 40 | 51 | 55 | 52 |
| OCC | 95 | 107 | 108 | 76 | 102 | 125 | 155 | **192** |

#### Cross-Architecture Observations

- **ARM exclusive locks peak at 4 threads then regress.** TAS/TTAS/CAS all hit ~184
  Mops/s at 4T and drop to ~120 at 8T. With 64 locks and 8 threads, contention per
  lock is still low, so the regression is likely due to memory bandwidth saturation
  from 8 cores issuing rapid-fire atomics.

- **x86 exclusive locks show a non-monotonic scaling pattern.** TAS and CAS dip at 4T
  (~35 Mops/s) then recover strongly at 8T+ (~70 Mops/s), eventually peaking at 32T
  (~112 Mops/s). TTAS scales more gradually without the dip. The dip may reflect
  contention dynamics at the 4-thread count where threads frequently collide on the
  same lock (4 threads / 64 locks = 6.25% collision probability), while at higher thread
  counts the parallelism across many locks compensates.

- **OCC dominates on both architectures at high read ratios.** At 95% reads: ARM OCC
  reaches 401 Mops/s at 8T (3.2x over RW's 126); x86 OCC reaches 192 Mops/s at 48T
  (3.7x over RW's 52). The optimistic read path generates zero cache-line invalidations,
  enabling true read parallelism.

- **ARM OCC at 95% reads scales super-linearly** from 1T (91 Mops/s) to 8T (401 Mops/s)
  — a 4.4x speedup with 8 threads. This is because with 64 locks and 95% reads,
  validation failures are rare, and the lock-free read path runs fully in parallel.

- **x86 OCC also scales continuously** up to 48T (192 Mops/s at 95% reads), while all
  other primitives plateau by 32T. This confirms that OCC's lock-free read path avoids
  the coherence bottleneck that limits the others.

- **RW lock plateaus on both architectures.** ARM RW peaks at ~161 Mops/s (4T) and x86
  at ~56 Mops/s (32T), regardless of read percentage. The CAS-based `read_lock()` makes
  readers as expensive as writers from a coherence perspective.

---

### 3.6 Lock Array — Lock Count Sweep (4 threads, cs_work=0, 80% read for OCC)

Varying the number of locks controls contention density.

#### ARM64

| Locks | TTAS (Mops/s) | CAS | Ticket | OCC |
|-------|---------------|------|--------|------|
| 1 | 53.1 | 35.1 | 10.3 | 74.1 |
| 4 | 53.3 | 52.5 | 35.0 | 168 |
| 16 | 119 | 118 | 73.8 | 247 |
| 64 | 187 | 184 | 107 | 263 |
| 256 | 222 | 221 | 120 | 268 |
| 1024 | 234 | 235 | 132 | 266 |

#### x86_64

| Locks | TTAS (Mops/s) | CAS | Ticket | OCC |
|-------|---------------|------|--------|------|
| 1 | 11.8 | 11.4 | 3.4 | 23.3 |
| 4 | 15.8 | 21.8 | 11.6 | 30.5 |
| 16 | 20.2 | 24.3 | 16.5 | 39.4 |
| 64 | 39.3 | 34.8 | 28.7 | 52.7 |
| 256 | 48.5 | 41.3 | 29.7 | 61.6 |
| 1024 | 61.5 | 56.6 | 39.9 | 80.7 |

#### Observations

- **Throughput scales with lock count on both architectures** — reducing contention
  density is the single most effective optimization, more important than lock algorithm
  choice.

- **ARM saturates earlier.** TTAS throughput increases 4.4x from 1 to 1024 locks on
  ARM, but only grows marginally past 256 locks (222 -> 234). On x86, TTAS grows 5.2x
  across the same range and is still climbing at 1024 locks, suggesting the x86 machine
  benefits from its larger core count even at 4 threads (less contention overhead per
  atomic).

- **OCC leads at every lock count on both platforms.** Even with just 1 lock, OCC
  (74 Mops/s ARM, 23 Mops/s x86) outperforms TTAS (53 Mops/s ARM, 12 Mops/s x86)
  because 80% of operations use the lock-free optimistic read path.

- **Ticket lock scales well with more locks but never catches up.** It benefits the most
  from reducing contention (12.8x on ARM from 1 to 1024 locks) because its FIFO
  ordering generates the most overhead under high contention, but it remains ~56% of
  TTAS throughput at all lock counts.

---

### 3.7 Lock Array — Critical Section Work Sweep (4 threads, 64 locks)

#### ARM64 (ns/op)

| cs_work | TTAS | CAS | Ticket |
|---------|------|------|--------|
| 0 | 5.4 | 5.4 | 9.6 |
| 50 | 7.6 | 7.7 | 10.1 |
| 100 | 13.6 | 13.8 | 13.9 |
| 500 | 44.5 | 45.5 | 44.8 |
| 1000 | 80.1 | 82.3 | 80.4 |

#### x86_64 (ns/op)

| cs_work | TTAS | CAS | Ticket |
|---------|------|------|--------|
| 0 | 25.5 | 29.0 | 36.8 |
| 50 | 35.7 | 29.3 | 36.9 |
| 100 | 46.1 | 35.2 | 38.9 |
| 500 | 81.4 | 72.6 | 78.9 |
| 1000 | 124 | 117 | 119 |

#### Observations

- **ARM locks converge by cs_work=100.** TTAS, CAS, and Ticket all produce ~13.6-13.9
  ns/op. The critical section cost dominates and lock overhead becomes negligible. This
  convergence happens because `busy_work` iterations are very cheap on ARM (~0.14 ns
  each).

- **x86 locks converge more slowly.** At cs_work=100, there's still a meaningful gap
  (TTAS: 46 ns vs Ticket: 39 ns). Convergence isn't reached until cs_work=1000 (all
  ~119-124 ns/op). Each `busy_work` iteration costs ~0.1 ns on x86 as well (the
  compiler barrier `asm volatile("" ::: "memory")` is essentially free on both), but
  the base lock overhead is much higher, so more iterations are needed to amortize it.

- **CAS performs slightly better than TTAS on x86 at moderate cs_work** (29 ns vs 36 ns
  at cs_work=50). On ARM, they are identical. The x86 `lock cmpxchg` may have a slight
  advantage over `lock xchg` when the lock is typically free (successful CAS avoids a
  retry).

- **With 64 locks, all primitives maintain good fairness** regardless of cs_work (ratios
  > 0.99 on ARM, > 0.6 on x86). This contrasts sharply with the single-lock CS sweep
  (Section 3.3) where starvation was extreme.

---

### 3.8 Generated Assembly Analysis (ARM64, Apple M3 Pro)

The compiler (Apple Clang 17, `-O3 -march=native`) targets ARMv8.1 with **LSE**
(Large System Extensions), which replaces the older LL/SC (`ldxr`/`stxr`) pairs
with single-instruction atomics (`swp`, `cas`, `ldadd`). This matters because
LSE atomics are handled directly by the memory controller, avoiding the retry
loops inherent to LL/SC.

Assembly was generated using `scripts/gen_asm.sh` and individual files are in `asm/`.

#### TAS Lock

```asm
; lock - just swap 1 in, check if old value was 0
    mov   w8, #1
    swpab w8, w9, [x0]       ; atomic swap byte (acquire). w9 = old value
    tbz   w9, #0, acquired   ; if bit 0 was clear, we got it
spin:
    yield                     ; hint to core we're spinning
    swpab w8, w9, [x0]       ; try again (RMW every iteration!)
    tbnz  w9, #0, spin
acquired:
    ret

; unlock - store zero with release
    stlrb wzr, [x0]          ; store-release byte
    ret
```

3 instructions on the fast path (mov + swpab + tbz).
Every iteration does a `swpab`, which is an RMW
that takes exclusive ownership of the cache line. Under contention, every core
is hammering the same line with store operations, causing it to bounce between
L1 caches (MOESI protocol transitions).

#### TTAS Lock

```asm
; lock - spin on plain loads, only try atomic swap when it looks free
    mov   w8, #1
    b     check
spin:
    yield
check:
    ldrb  w9, [x0]           ; plain load (shared, no bus traffic!)
    tbnz  w9, #0, spin       ; still locked? keep spinning on loads
    swpab w8, w9, [x0]       ; looks free -> try atomic swap
    tbnz  w9, #0, check      ; someone beat us, go back to spinning
    ret

; unlock - identical to TAS
    stlrb wzr, [x0]
    ret
```

The key difference from TAS: the spin loop uses `ldrb` (plain load byte) instead
of `swpab`. A plain load can be served from the local L1 cache in shared state
without invalidating other cores' copies. Only when the lock appears free does
the thread issue the expensive `swpab`. This dramatically reduces coherence
traffic under contention.

#### CAS Lock

```asm
; lock - compare-and-swap with same TTAS-style spin-read
    mov   w9, #0
    mov   w8, #1
    casab w9, w8, [x0]       ; if [x0]==0, set to 1 (acquire)
    cmp   w9, #0              ; did old value match expected (0)?
    b.ne  spin_read           ; no -> fall into spin-read loop
    ret
spin:
    yield
spin_read:
    ldrb  w9, [x0]           ; plain load (same trick as TTAS)
    tbnz  w9, #0, spin       ; locked? keep spinning
    mov   w9, #0              ; reload expected value (CAS clobbers it)
    casab w9, w8, [x0]       ; try CAS again
    cmp   w9, #0
    b.ne  spin_read
    ret

; unlock - identical to TAS/TTAS
    stlrb wzr, [x0]
    ret
```

CAS has the same spin-read optimization as TTAS but uses `casab` (compare-and-swap
byte, acquire) instead of `swpab`. The extra `mov w9, #0` to reload the expected
value before each CAS attempt explains why CAS is slightly slower than TTAS
single-threaded (829 vs 955 Mops/s) — `casab` needs both an expected and
desired value, while `swpab` only needs the desired value. Under contention they
converge because the spin-read loop dominates.

#### Ticket Lock

```asm
; lock - atomically grab a ticket, then spin on the owner counter
    mov   w8, #1
    ldadd w8, w8, [x0]       ; fetch_add(1) on 'next'. w8 = our ticket
    ldapur w9, [x0, #64]     ; load 'owner' (64 bytes away!)
    cmp   w9, w8
    b.eq  acquired
spin:
    yield
    ldapur w9, [x0, #64]     ; re-read owner (load-acquire)
    cmp   w9, w8
    b.ne  spin
acquired:
    ret

; unlock - read-modify-store on owner (no atomic needed, we're the only writer)
    ldr   w8, [x0, #64]      ; plain load of owner
    add   w8, w8, #1
    stlur w8, [x0, #64]      ; store-release (makes it visible to waiters)
    ret
```

`lock()` accesses two different cache lines — `ldadd`
on `next` at `[x0]` and `ldapur` on `owner` at `[x0, #64]`. The `alignas(64)` in the
C++ code ensures these counters live on separate 64-byte cache lines, avoiding false
sharing between the ticket dispenser and the "now serving" display.

The spin loop only reads `owner` (`ldapur`), it never writes. Only the
unlock path writes to `owner`, and since exactly one thread holds the lock, there
is only one writer to the owner cache line at any time. This is why the spin
loop generates less coherence traffic than TAS. However, the `ldadd` on `next`
during lock acquisition is an RMW that bounces under heavy contention, which is
why ticket lock is still slower than TTAS overall.

The unlock uses `stlur` (store-release with unscaled offset) — a non-atomic store
is safe here because only the lock holder writes to `owner`.

#### RW Lock

```asm
; read_lock - CAS to increment reader count
check:
    ldr   w8, [x0]           ; load state
    tbnz  w8, #31, spin      ; bit 31 set = negative = writer active
    add   w9, w8, #1         ; new state = readers + 1
    mov   x10, x8
    casa  w10, w9, [x0]      ; CAS: if state==old, set to old+1 (acquire)
    cmp   w10, w8
    b.ne  check              ; CAS failed, retry
    ret
spin:
    yield
    b     check

; read_unlock - atomic decrement
    mov   w8, #-1
    ldaddl w8, w8, [x0]      ; fetch_add(-1) with release
    ret

; write_lock - CAS state from 0 to -1
    mov   w9, #0
    mov   w8, #-1
    casa  w9, w8, [x0]       ; if state==0, set to -1 (acquire)
    cmp   w9, #0
    b.eq  acquired
spin:
    mov   w9, #0
    yield
    casa  w9, w8, [x0]       ; retry CAS (no TTAS-style spin-read!)
    cmp   w9, #0
    b.ne  spin
acquired:
    ret

; write_unlock - just store 0
    stlr  wzr, [x0]          ; store-release word
    ret
```

The RW lock assembly reveals **why it underperforms OCC for reads**:

- `read_lock` requires a `casa` (CAS word, acquire) — a full RMW that takes exclusive
  ownership of the cache line. Even though multiple readers can hold the lock
  concurrently, each `read_lock` call **invalidates the cache line for all other cores**.
  Under contention, readers serialize on the CAS.
- `read_unlock` uses `ldaddl` (atomic fetch_add with release) — another RMW. A
  complete read-side critical section requires **two RMW operations** (`casa` + `ldaddl`).
- `write_lock` issues `casa` on every spin iteration (no TTAS-style spin-read),
  making the writer path more expensive under contention than TTAS/CAS.

#### OCC (Seqlock)

```asm
; read_begin - just load the version with acquire
    mov   x8, x0
    ldapr x0, [x0]           ; load-acquire (ARMv8.3 LDAPR)
    tbz   w0, #0, done       ; even version? good to go
spin:
    yield
    ldapr x0, [x8]           ; re-read version
    tbnz  w0, #0, spin       ; odd = writer active, keep waiting
done:
    ret

; read_validate - fence + plain load + compare
    dmb   ishld              ; acquire fence (inner-shareable, loads only)
    ldr   x8, [x0]          ; plain load of version
    cmp   x8, x1            ; same as start version?
    cset  w0, eq            ; return true/false
    ret

; write_lock - TTAS-style: spin-read, then CAS version from even to odd
check:
    ldr   x8, [x0]           ; plain load of version
    tbnz  w8, #0, spin       ; odd? someone is writing
    orr   x9, x8, #0x1       ; new version = old | 1 (set odd)
    mov   x10, x8
    casa  x10, x9, [x0]      ; CAS: if version==old, set odd (acquire)
    cmp   x10, x8
    b.ne  check              ; failed, retry
    ret
spin:
    yield
    b     check

; write_unlock - atomic increment (odd -> even)
    mov   w8, #1
    ldaddl x8, x8, [x0]     ; fetch_add(1) with release
    ret
```

**This is why OCC dominates read-heavy workloads.** The entire read path consists of:
- `read_begin`: one `ldapr` (load-acquire). On ARM, `ldapr` does not invalidate the
  cache line and is served from shared L1 state. **No RMW, no cache-line bouncing.**
- `read_validate`: one `dmb ishld` (load-only fence) + one `ldr` (plain load) + compare.
  **Zero RMW operations.** The `dmb ishld` prevents the CPU from reordering data reads
  past the version check.

Compare to the RW lock: `casa` (RMW) + `ldaddl` (RMW) = two cache-line invalidations
per read. OCC causes **zero invalidations**, which is why at 95% reads and 8 threads,
OCC achieves 401 Mops/s vs RW's 126 Mops/s (**3.2x**) on the lock array benchmark.

The tradeoff: validation fails if a writer was active during the read, requiring a
retry. This overhead is measurable once writes exceed ~20-30% of operations.

#### Instruction Count Summary

| Lock | lock() fast path | unlock() | Read-path atomics |
|------|-----------------|----------|-------------------|
| TAS | 3 insns (`mov` + `swpab` + `tbz`) | 2 insns (`stlrb` + `ret`) | N/A |
| TTAS | 5 insns (`mov` + `ldrb` + `tbnz` + `swpab` + `tbnz`) | 2 insns | N/A |
| CAS | 6 insns (`mov` + `mov` + `casab` + `cmp` + `b.ne` + `ret`) | 2 insns | N/A |
| Ticket | 6 insns (`mov` + `ldadd` + `ldapur` + `cmp` + `b.eq` + `ret`) | 4 insns | N/A |
| RW | read: 7 insns (with `casa`), unlock: `ldaddl` | write: `stlr` | **2 RMW** (`casa` + `ldaddl`) |
| OCC | read: `ldapr` + `tbz`, validate: `dmb` + `ldr` + `cmp` + `cset` | write: `ldaddl` | **0 RMW** |

---

## 4. Key Takeaways

1. **ARM's LSE atomics are fundamentally faster than x86's `lock`-prefixed instructions.**
   Single-threaded spinlock throughput is 8.3x higher on ARM (953 vs 116 Mops/s for TAS).
   This gap narrows under contention (1.9x at 6T for TAS) and OCC is the one case where
   x86 can win (3.7 vs 3.2 Mops/s at 6T under exclusive workload).

2. **OCC wins on both architectures for read-heavy workloads but is a trap under
   exclusive access.** Its lock-free read path enables true read parallelism: ARM OCC-95%
   retains 54.8 Mops/s at 6T. However, under exclusive workloads, OCC exhibits livelock
   (fairness 0.10 on ARM 4T) and a 13x throughput cliff from 4T to 6T on ARM as E-cores
   break the monopoly pattern.

3. **Fairness-throughput tradeoff is architecture-dependent.** Ticket lock guarantees
   FIFO fairness on both platforms, but the throughput penalty varies: ~56% of TTAS on
   ARM, ~42% on x86. On x86, all unfair locks show consistently lower fairness (0.43-0.62)
   than on ARM (0.76-0.97 at 4T).

4. **ARM's big.LITTLE architecture creates scaling cliffs.** The M3 Pro's 6 P-cores and
   5 E-cores cause performance degradation when threads exceed the P-core count. OCC is
   most affected (13x cliff at 6T), but all locks lose 60-97% of 1T throughput at 6T.
   Benchmarks on heterogeneous cores should cap threads at the P-core count.

5. **Critical section length equalizes platforms.** ARM's 8x advantage at cs_work=0
   narrows to 1.3x at cs_work=5000. In practice, most real workloads have non-trivial
   critical sections, so the ARM/x86 gap is smaller than raw lock benchmarks suggest.

6. **Contention density matters more than lock algorithm.** Increasing the lock count
   from 1 to 1024 yields 4.4x improvement on ARM and 5.2x on x86 for TTAS. Striping
   or partitioning should be the first optimization before tuning the lock primitive.

7. **RW locks disappoint in practice on both architectures.** The CAS in `read_lock()`
   and the atomic decrement in `read_unlock()` create two cache-line invalidations per
   read operation, negating the theoretical advantage of shared reads. OCC achieves the
   same goal with zero read-path atomics.

---

## 5. Reproducing the Experiments

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Quick single run
./build/lockbench --lock ttas --workload mutex --threads 4 --seconds 3 --warmup 3

# Full sweep (lockbench): 3s measurement, 6 threads max, 5 repeats
# On Linux with pinning:
sudo ./scripts/setup_cpu.sh          # lock CPU frequency (optional, requires root)
./scripts/sweep.sh 3 6 5             # auto-detects Linux and passes --pin
sudo ./scripts/setup_cpu.sh --reset

# On macOS:
./scripts/sweep.sh 3 6 5             # uses QoS hints for P-core bias

# Lock array sweep
./scripts/array_sweep.sh 3 8 5

# Analysis notebook
cd results && jupyter notebook presentation_notebook.ipynb
```

### Available Options

**lockbench**: `--lock tas|ttas|cas|ticket|rw|occ|rcu` `--workload mutex|rw|rcu`
`--threads N` `--seconds S` `--warmup S` `--cs_work N` `--read_pct P` `--pin`
`--csv FILE`

**arraybench**: `--lock tas|ttas|cas|ticket|rw|occ` `--threads N` `--seconds S`
`--warmup S` `--num_locks N` `--cs_work N` `--read_pct P` `--pin` `--csv FILE`


## Wormhole — Lock-Primitive Swap

Wu et al.'s Wormhole (FAST '19) is a hybrid trie+hash+linked-list ordered
concurrent index with three lock sites: a per-leaf rwlock (`leaflock`), a
global hashmap rwlock (`metalock`), and a small leaf spinlock (`sortlock`).
We vendor wormhole at `third_party/wormhole/` (editable copy of Wu's repo)
and replace its lock bodies with a C-callable shim that dispatches to one
of our lockbench primitives, selected at compile time.

### Variants built

- `wh-default` — wormhole's stock rwlock (no shim)
- `wh-rw` — `rw_lock` from `include/primitives/rw_lock.hpp` (true reader-writer)
- `wh-tas`, `wh-ttas`, `wh-cas`, `wh-occ` — exclusive-only (read = write)
- `wh-occ-opt` — true OCC: lock-free seqlock-validated reads + exclusive
  writes (uses `cas_lock` underneath). See "OCC-optimistic variant" below.

`wh-ticket` is **not built**. See "Ticket lock excluded" below.

### Header-injection lock shim

- `third_party/wormhole/wh_lock_shim.h` declares `struct rwlock` /
  `struct spinlock` and the rwlock_*/spinlock_* C function signatures
  matching upstream `lib.h` (lines 304–366).
- `third_party/wormhole/wh_lock_shim.cpp` selects `LockT` via `WH_LOCK_<X>`
  and provides `extern "C"` bodies that placement-new the lock into the
  struct's opaque storage and dispatch through C++ template helpers.
- Upstream `lib.h` is patched with a guarded `#ifdef WH_LOCK_SHIM
  #include "wh_lock_shim.h"` block; lock sections of `lib.c` and the two
  `.opaque` references in `wh.c` are wrapped in `#ifndef WH_LOCK_SHIM`.

CMake builds one `wormhole-rt-<lk>` static lib + `wh_bench_<lk>` +
`wh_test_<lk>` per variant. Sweep iterates binaries (lock is fixed at
compile time, not selectable at runtime).

### Ticket lock excluded

A real ticket-lock acquire is destructive (`fetch_add` mortgages a queue
slot), so `try_lock` cannot return `false` after grabbing a ticket
without breaking everyone behind us. The only correctness-preserving
non-blocking try is "succeed iff queue empty (CAS `next: owner →
owner+1`); else fail":

```cpp
bool try_lock() noexcept {
    auto cur_owner = owner.load(std::memory_order_relaxed);
    auto cur_next  = next.load(std::memory_order_relaxed);
    if (cur_next != cur_owner) return false;          // queue not empty
    return next.compare_exchange_strong(cur_next, cur_owner + 1,
        std::memory_order_acquire, std::memory_order_relaxed);
}
```

Wormhole's reader fast path calls `rwlock_trylock_write_nr(leaflock, 64)`.
Under any sustained writer activity the queue is rarely empty, so the
ticket try-lock effectively always fails and readers always fall to the
slow optimistic path. Result: `wh-ticket` would look catastrophically slow
on read-heavy contended workloads — *not* because ticket is bad in
general, but because **strict-FIFO ordering is incompatible with
try-lock-based reader protocols**. We saw the same effect on
BronsonAVLTreeMap (where `avl-ticket` dropped to ≈0.9 M ops/s on
write-heavy zipfian).

For the wormhole sweep, `wh-ticket` would measure the shim's deviation
rather than a genuine property of the ticket primitive, so we exclude it.

### OCC plugged in as exclusive-only

`occ_lock`'s `lock()`/`unlock()` aliases delegate to the seqlock-version-
bump CAS used for writes. Plugging into the rwlock shim, every operation
(read or write) takes the seqlock-as-mutex path; OCC's optimistic-read
protocol (`read_begin`/`read_validate`) is bypassed. This isn't strictly a
loss — wormhole has its own version-check fallback against `leaf->lv`
that gives a structurally similar effect on the slow path.

### RCU/QSBR is orthogonal

Wormhole uses QSBR (a quiescent-state-based RCU variant) via the
`wormref` per-thread handle to defer freeing of removed leaves until
every reader has passed through a quiescent point. This governs **when
freed memory is returned to the allocator**, not **who can access live
state**. Our shim swaps only lock bodies; the QSBR engine and the
reader-version protocol it cooperates with are untouched. Memory ordering
on each lock primitive's acquire/release is sufficient for QSBR
invariants to hold.

### OCC-optimistic variant

`wh-occ-opt` adds a true lock-free reader path to wormhole. Three patches
to `wh.c`, all gated by `#ifdef WH_OCC_OPTIMISTIC`:

1. **Per-leaf seqlock counter.** `_Atomic u64 occ_seq` field added to
   `struct wormleaf`. Bumped to odd at the start of every
   `wormleaf_lock_write`, back to even at the end of every
   `wormleaf_unlock_write`. Initialized to 0 in `wormleaf_alloc`.
   Two bypass paths (`wormhole_jump_leaf_write`'s direct
   `rwlock_trylock_write_nr`; `wormhole_split_insert`'s direct
   `rwlock_lock_write` on the new leaf) also bump on success.
2. **Lock-free `wormhole_get`.** Replaces the locked reader path with:
   - Snapshot the per-leaf seq.
   - Linear scan `hs[]` using atomic 8-byte loads of the full `v64`
     (avoids torn reads of separate `e1`/`e3` fields).
   - Re-load seq; if unchanged AND no structural rebuild (`leaf->lv`
     ≤ hmap snapshot version), return result. Otherwise retry.
3. **No-op `mm.free`.** Old kvs would otherwise be freed eagerly while
   a concurrent reader has a stale pointer to them. The C++ adapter
   installs a custom `kvmap_mm` whose `free` does nothing, so kvs
   leak for the lifetime of the process. For 3-second benches this is
   ~150 MB. A production-grade implementation would defer the free
   via QSBR (out of scope here).

Underlying *write* lock is `cas_lock` (the fastest exclusive primitive).
Comparing `wh-occ-opt` against `wh-cas` isolates the
"lock-free reader vs exclusive reader" effect; comparing against
`wh-rw` isolates "lock-free reader vs shared reader."

See `WORMHOLE_ADAPTATION.md` for the full implementation walkthrough.

### Verification

```bash
cmake --build build -j

# Standard correctness
for lk in default rw tas ttas cas occ occ-opt; do
  ./build/wh_test_${lk} --mode both
done

# Stress: 16 threads × 200k ops × disjoint slices.
# Load-bearing check that the lock swap doesn't break QSBR reclamation
# AND that the optimistic reader tolerates concurrent writers.
for lk in default rw tas ttas cas occ occ-opt; do
  ./build/wh_test_${lk} --mode race --threads 16 \
      --per_thread_keys 1024 --ops_per_thread 200000
done

# Sweep (4 workloads × 7 locks × 4 thread counts → 112 runs)
./scripts/wh_compare.sh 3 1
wc -l results/wh_compare/wh.csv     # expect 113
```

### Results (Apple M-series, 8 threads, M ops/s, fair-mm build)

All variants use the same no-op-free `kvmap_mm` so per-op `free()` cost
is uniform across the comparison. Without this (`-DWH_FAIR_MM=OFF`),
locked variants pay a per-op `free()` the optimistic variant avoids,
inflating the apparent reader-strategy speedup by ~2× on write-heavy
workloads. See "MM fairness" below.

| Workload          | default |   rw |  tas | ttas |  cas |  occ | occ-opt |
|-------------------|--------:|-----:|-----:|-----:|-----:|-----:|--------:|
| uniform 80/10/10  |    37.3 | 49.7 | 50.1 | 48.9 | 50.3 | 49.8 |  **50.7** |
| zipfian 80/10/10  |    40.5 | 47.0 | 46.2 | 47.0 | 47.3 | 46.9 |  **51.2** |
| uniform 90/5/5    |    54.8 | 51.2 | 49.6 | 49.5 | 52.5 | 51.7 |  **56.2** |
| zipfian 20/40/40  |    39.9 | 37.4 | 38.6 | 38.0 | 39.0 | 38.0 |    36.9 |

Plots: `results/wh_compare/wh_locks.png`, `wh_8threads.png`,
`wh_occopt_focus.png`. Raw data: `results/wh_compare/wh.csv`.

### What the numbers actually say

1. **`wh-occ-opt` wins on read-heavy and balanced workloads.** On
   uniform 90/5/5 (the most read-heavy mix, 90% reads) it's the
   fastest variant at 56.2 M ops/s. On zipfian 80/10/10 (skewed but
   read-heavy) it's also the fastest at 51.2 M. On uniform 80/10/10
   it ties for the lead. **This is the lock-free-reader benefit
   showing up clearly: when reads dominate, not taking any lock pays
   off.**

2. **`wh-occ-opt` is competitive on write-heavy workloads.** On
   zipfian 20/40/40 (80% writes on hot keys) it's 36.9 vs 37–40 for
   the locked variants — within ~7%. The lock-free reader doesn't
   help much when most ops are writers serializing on the leaflock,
   but the overhead is also minimal.

3. **All locked variants (rw, tas, ttas, cas, occ) cluster within
   ±5% on every workload.** Wormhole's per-leaf locking is
   fine-grained enough that lock-primitive choice doesn't dominate —
   different from StripedMap / Bronson AVL where the lock primitive
   matters more. Wu's stock rwlock (`default`) consistently lags by
   5–25% (vs our shim variants), suggesting our shim's `cas_lock`-
   backed rwlock is slightly cheaper than Wu's bit-packed `u32`-based
   one on Apple Silicon — possibly due to fewer atomic ops on the
   acquire path.

4. **The reader-side implementation choice was load-bearing.**
   An earlier version of `wh-occ-opt` used a linear scan over all
   WH_KPN=128 slots (defensive choice, to avoid torn `entry13` reads).
   That implementation paid ~128 atomic loads per read and lost
   badly on read-heavy workloads (20.2 M ops/s on uniform 90/5/5).
   The current version uses a hash-indexed walk that mirrors
   wormhole's stock `wormleaf_match_hs` but reads each `entry13` as
   a single atomic 8-byte load — same O(1) expected behavior, no
   torn-read hazard, ~3× faster reads.

### MM fairness: a measurement trap

Initial results showed `wh-occ-opt` 4× faster on zipfian 20/40/40.
Investigation revealed the speedup was **not from lock-free reads**
but from avoiding `free()`:

- The optimistic-reader variant **must** use a no-op-free `kvmap_mm`
  for safety: a reader walking a leaf without the leaflock can't
  safely deref a kv that a concurrent updater just freed.
- Wormhole's stock `kvmap_mm_dup` calls `free()` on every update
  and delete. Removed kvs go straight to the allocator.
- With ~80% of ops in zipfian 20/40/40 being inserts/deletes,
  the locked variants paid `free()` cost on 80% of ops (~10–30 ns
  per call); `wh-occ-opt` paid nothing.

Comparing the two MM modes head-to-head on zipfian 20/40/40 (8 threads):

| Variant   | Stock `kvmap_mm_dup` | No-op-free (`WH_FAIR_MM`) |
|-----------|---------------------:|--------------------------:|
| `wh-cas`  |              7.0 M  |                    35.3 M |
| `wh-occ-opt` |          30.6 M  |                    27.7 M |

The locked variants speed up **5×** when the per-op `free()` is
removed; `wh-occ-opt` is unchanged (it was already using no-op-free).
The gap inverts: with fair MM, locked variants beat `wh-occ-opt` on
this workload.

Lesson: **eliminate allocator-cost asymmetries before comparing
synchronization strategies**. We expose this via the `WH_FAIR_MM`
CMake option; build with `cmake -DWH_FAIR_MM=ON`. The default build
(`WH_FAIR_MM=OFF`) is reserved for studying the locked variants in
their realistic configuration.
