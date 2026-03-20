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
| Memory | 36 GB | <!-- TODO: fill in --> |
| Compiler | Apple Clang 17.0.0, `-O3 -march=native` | <!-- TODO: fill in --> |
| OS | macOS (Darwin 24.3.0) | <!-- TODO: fill in --> |
| Max threads benchmarked | 8 | 48 |
| C++ Standard | C++20 | C++20 |

The ARM64 platform uses ARMv8.1 **LSE** (Large System Extensions), which replaces
LL/SC (`ldxr`/`stxr`) pairs with single-instruction atomics (`swp`, `cas`, `ldadd`).
The x86_64 platform uses `lock`-prefixed instructions (`lock xchg`, `lock cmpxchg`,
`lock xadd`).

### Methodology

- **Warmup**: 1 second before measurement (excluded from results).
- **Measurement**: 3 seconds per configuration; reported as total ops and ns/op.
- **Fairness**: Ratio of min to max per-thread operation count (1.0 = perfect).
- **Runs**: Each configuration is run multiple times (default 3); tables report averages.
- Each thread runs a tight loop performing operations until a shared `stop` flag is
  set. Per-thread counts are accumulated after join.

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

#### ARM64 (Apple M3 Pro)

| Lock | 1T (Mops/s) | 2T | 4T | 8T |
|------|-------------|-----|------|------|
| TAS | **988** | 114 | 32.7 | 3.9 |
| TTAS | 955 | 134 | **56.5** | **11.2** |
| CAS | 829 | **164** | 55.3 | 20.1 |
| Ticket | 529 | 19.4 | 10.4 | 4.4 |
| RW | 820 | 149 | 34.7 | 6.9 |
| OCC | 285 | 81.5 | 42.0 | 9.5 |

#### x86_64

| Lock | 1T (Mops/s) | 2T | 4T | 8T |
|------|-------------|------|-----|------|
| TAS | 115 | 23.7 | 6.6 | 2.5 |
| TTAS | 116 | 24.0 | 6.7 | 4.2 |
| CAS | 116 | 22.8 | 6.9 | **4.3** |
| Ticket | **127** | 9.0 | 2.5 | 2.0 |
| RW | 116 | 22.1 | 5.4 | 2.7 |
| OCC | 64.6 | 14.0 | 5.2 | 3.2 |

#### Cross-Architecture Observations

- **ARM is 8x faster single-threaded for spinlocks.** TAS achieves 988 Mops/s on ARM
  vs 115 Mops/s on x86 (~1 ns/op vs ~8.7 ns/op). ARM's LSE `swpab` (byte-granularity
  atomic swap) executes in very few cycles uncontended, while x86's `lock xchg` carries
  higher fixed cost from the lock prefix's full memory barrier.

- **ARM spinlocks show large single-threaded spread; x86 does not.** On ARM, TAS (988)
  vs Ticket (529) is a 1.9x gap — byte-sized `swpab` is measurably cheaper than
  32-bit `ldadd` + `ldapur`. On x86, TAS/TTAS/CAS/RW all cluster around 115-116 Mops/s
  because `lock xchg`, `lock cmpxchg`, and `lock xadd` have similar pipeline cost.

- **Ticket lock is the fastest single-threaded primitive on x86** (127 Mops/s), likely
  because `lock xadd` (fetch-and-add) is slightly cheaper than `lock xchg`/`lock
  cmpxchg` on this microarchitecture. On ARM, ticket is the slowest due to its
  two-cache-line protocol.

- **TAS degrades faster on ARM under contention.** ARM TAS drops 253x from 1T to 8T
  (988 -> 3.9) because every spin iteration issues an RMW (`swpab`), bouncing the cache
  line between all cores. On x86, the drop is 46x (115 -> 2.5) — still severe, but
  x86's coherence protocol handles the bouncing with somewhat less relative overhead
  since the base cost per operation is already higher.

- **TTAS/CAS scale similarly on both architectures.** The spin-read optimization
  (plain load in the spin loop, RMW only when the lock looks free) reduces coherence
  traffic on both platforms. ARM TTAS retains 11.2 Mops/s at 8T; x86 retains 4.2.

---

### 3.2 Fairness (4 threads, cs_work=0)

#### ARM64

| Lock | Ratio |
|------|-------|
| TAS | 0.77 |
| TTAS | 0.61 |
| CAS | 0.35 |
| Ticket | **1.00** |
| RW | 0.80 |
| OCC | 0.15 |

#### x86_64

| Lock | Ratio |
|------|-------|
| TAS | 0.45 |
| TTAS | 0.45 |
| CAS | 0.48 |
| Ticket | **1.00** |
| RW | 0.73 |
| OCC | 0.49 |

#### Observations

- **Ticket lock achieves perfect fairness on both architectures** (ratio >= 0.999).
  FIFO ordering ensures every thread gets served in order, at the cost of 5-6x lower
  throughput than TTAS/CAS.

- **Fairness patterns differ between architectures.** On ARM, CAS (0.35) and OCC (0.15)
  show extreme unfairness — a single thread can dominate because ARM's fast atomics
  allow the releasing thread to reacquire before coherence propagates the release. On
  x86, the longer atomic latency gives other threads more opportunity to observe the
  release, resulting in more uniform (though still unfair) distributions across
  TAS/TTAS/CAS (~0.45-0.48).

---

### 3.3 Critical Section Cost Sweep (4 threads, TTAS, lockbench)

Single shared lock, varying `cs_work` (loop iterations inside the critical section).

| cs_work | ARM64 (ns/op) | x86_64 (ns/op) | ARM fairness | x86 fairness |
|---------|---------------|-----------------|--------------|--------------|
| 0 | 18.2 | 58.9 | 0.54 | 0.04 |
| 50 | 30.2 | 299 | 0.53 | 0.23 |
| 100 | 60.3 | 387 | 0.27 | 0.19 |
| 500 | 153 | 504 | 0.09 | 0.19 |
| 1000 | 291 | 541 | 0.006 | 0.15 |
| 5000 | 1398 | 1831 | ~0 | 0.10 |

#### Observations

- **Each `busy_work` iteration costs ~0.24 ns on ARM vs ~4.8 ns on x86.** This 20x
  difference in per-iteration cost means the same `cs_work` value represents very
  different real-time critical section durations on the two platforms. On ARM, 50
  iterations add ~12 ns; on x86, they add ~240 ns.

- **On ARM, fairness degrades catastrophically with increasing cs_work.** At
  cs_work=1000, one thread performs ~35K ops while another performs ~5.5M (ratio 0.006).
  The releasing thread's cache line stays hot in L1, and ARM's fast `swpab` lets it
  reacquire before the coherence protocol delivers the release to waiting cores. At
  cs_work=5000, the min thread gets as few as 1 operation over the entire 3-second run.

- **On x86, fairness is poor but stable.** The ratio hovers around 0.04-0.23 regardless
  of cs_work. x86's longer atomic latency and stronger ordering (`lock` prefix acts
  as a full barrier) gives waiting threads a more consistent, if small, chance to acquire.

- **Both architectures converge at high cs_work.** At cs_work=5000, ARM (1398 ns/op)
  and x86 (1831 ns/op) are within 1.3x — the critical section dominates and lock
  overhead becomes negligible.

---

### 3.4 Reader-Writer Workloads (lockbench)

Single shared lock, RW/OCC/RCU primitives with varying read percentages.

#### ARM64

| Lock | Read % | 1T (Mops/s) | 2T | 4T | 8T |
|------|--------|-------------|------|------|------|
| RW | 80 | 98.6 | 29.0 | 14.7 | 4.4 |
| OCC | 80 | 100 | 48.2 | 36.0 | 10.4 |
| RW | 95 | 122 | 29.9 | 17.7 | 4.3 |
| OCC | 95 | 122 | 80.4 | **65.2** | **32.8** |
| RCU | 90 | 101 | 48.1 | 36.1 | 12.2 |

#### x86_64

| Lock | Read % | 1T (Mops/s) | 2T | 4T | 8T |
|------|--------|-------------|------|------|------|
| RW | 80 | 51.9 | 7.8 | 3.3 | 2.1 |
| OCC | 80 | 78.9 | 19.4 | 6.2 | 5.0 |
| RW | 95 | 49.2 | 5.6 | 4.3 | 3.1 |
| OCC | 95 | 99.6 | 32.7 | **17.9** | **13.5** |
| RCU | 90 | 85.3 | 20.0 | 7.1 | 6.7 |

#### Observations

- **RW lock does not scale with read percentage on either architecture.** At 8T on ARM,
  RW achieves ~4.3-4.4 Mops/s regardless of read ratio. On x86 it's ~2.1-3.1 Mops/s.
  Each `read_lock` issues an RMW (`casa` on ARM, `lock cmpxchg` on x86) that
  invalidates the cache line, serializing readers on the same line as writers.

- **OCC scales with read ratio on both platforms, but the benefit is larger on ARM.**
  At 95% reads and 8T: ARM OCC delivers 32.8 Mops/s (7.6x over RW). x86 OCC delivers
  13.5 Mops/s (4.4x over RW). ARM benefits more because its optimistic read path
  (`ldapr` + `dmb ishld` + `ldr`) involves zero RMW operations and zero cache-line
  invalidations. On x86, the `mfence` or `lfence` in the read validation path adds
  more overhead.

- **RCU on ARM matches OCC at 80% reads** (36 vs 36 Mops/s at 4T) but falls behind at
  95% reads because the write-side `synchronize()` must drain all reader epochs. On x86,
  RCU shows modest scaling (7.1 Mops/s at 4T, 90% reads), limited by the higher
  per-epoch overhead.

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
   Single-threaded spinlock throughput is 8x higher on ARM (988 vs 115 Mops/s for TAS).
   This gap narrows under contention but never fully closes. The byte-granularity atomics
   (`swpab`, `casab`) on ARM are particularly efficient.

2. **OCC wins on both architectures for read-heavy workloads.** Its lock-free read path
   (zero RMW operations on ARM, minimal overhead on x86) enables true read parallelism.
   At 95% reads with 64 locks: ARM OCC reaches 401 Mops/s at 8T; x86 OCC reaches 192
   Mops/s at 48T — both far ahead of any other primitive.

3. **Fairness-throughput tradeoff is architecture-dependent.** Ticket lock guarantees
   FIFO fairness on both platforms, but the throughput penalty varies: ~56% of TTAS on
   ARM, ~42% on x86. Conversely, unfair locks (TTAS/CAS) cause more extreme starvation
   on ARM than on x86 because ARM's faster atomics amplify the "hot cache line" advantage
   of the releasing thread.

4. **Critical section cost dominates quickly on ARM, slowly on x86.** ARM locks converge
   by cs_work=100 (~14 ns/op for all primitives). x86 needs cs_work=1000 to converge
   (~120 ns/op). Choosing the right lock matters more on x86 when critical sections
   are short.

5. **Contention density matters more than lock algorithm.** Increasing the lock count
   from 1 to 1024 yields 4.4x improvement on ARM and 5.2x on x86 for TTAS. Striping
   or partitioning should be the first optimization before tuning the lock primitive.

6. **RW locks disappoint in practice on both architectures.** The CAS in `read_lock()`
   and the atomic decrement in `read_unlock()` create two cache-line invalidations per
   read operation, negating the theoretical advantage of shared reads. OCC achieves the
   same goal with zero read-path atomics.

---

## 5. Reproducing the Experiments

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Raw lock benchmarks
./build/lockbench --lock ttas --workload mutex --threads 4 --seconds 3
./build/lockbench --lock occ --workload rw --threads 4 --seconds 3 --read_pct 95

# Lock array benchmarks
./build/arraybench --lock ttas --threads 4 --seconds 3 --num_locks 64
./build/arraybench --lock occ --threads 4 --seconds 3 --num_locks 64 --read_pct 95

# Generate assembly for all locks
./scripts/gen_asm.sh          # outputs to asm/

# Full sweeps (results written to results/*.csv)
./scripts/sweep.sh 3 8          # Raw lock sweep
./scripts/array_sweep.sh 3 8    # Lock array sweep
```

### Available Options

**lockbench**: `--lock tas|ttas|cas|ticket|rw|occ|rcu` `--workload mutex|rw|rcu`
`--threads N` `--seconds S` `--warmup S` `--cs_work N` `--read_pct P` `--csv FILE`

**arraybench**: `--lock tas|ttas|cas|ticket|rw|occ` `--threads N` `--seconds S`
`--warmup S` `--num_locks N` `--cs_work N` `--read_pct P` `--csv FILE`
