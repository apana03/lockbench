# Lockbench: Synchronization Primitives for Concurrent Indexes

## 1. Objective

The goal is to determine which locking
strategy adds minimal overhead for index operations (lookups, inserts, deletes)
under varying contention levels, thread counts, and access patterns.

The primitives studied are:

| Primitive | Type | Mechanism |
|-----------|------|-----------|
| **TAS** (Test-And-Set) | Spinlock | `atomic_flag::test_and_set` in a tight loop |
| **TTAS** (Test-and-Test-And-Set) | Spinlock | Spin-read (shared), then attempt exchange (RMW) |
| **CAS** (Compare-And-Swap) | Spinlock | `compare_exchange_weak` with TTAS-style spin-read |
| **Ticket Lock** | Spinlock (FIFO) | Two counters: `next` (ticket dispenser), `owner` (now serving) |
| **RW Lock** | Reader-Writer | Atomic state: count ≥ 0 for readers, −1 for writer |
| **OCC** (Optimistic Concurrency) | Seqlock | Version counter: even = consistent, odd = write in progress |
| **RCU** (Read-Copy-Update) | Epoch-based | Per-thread epoch announcement; writers synchronize via epoch drain |

## 2. Experimental Setup

### Platform

| Component | Value |
|-----------|-------|
| CPU | Apple M3 Pro (ARM64, 11 cores) |
| Memory | 36 GB |
| Compiler | Apple Clang 17.0.0, `-O3 -march=native` |
| OS | macOS (Darwin 24.3.0) |
| C++ Standard | C++20 |

### Methodology

- **Warmup**: 1 second before measurement (excluded from results).
- **Measurement**: 3 seconds per configuration; reported as total ops and ns/op.
- **Fairness**: Ratio of min to max per-thread operation count (1.0 = perfect).
- Each thread runs a tight loop performing operations until a shared `stop` flag is
  set. Per-thread counts are accumulated after join.

### Benchmarks

1. **lockbench** (raw): Threads repeatedly acquire and release a lock with no data
   structure work. Measures pure lock overhead.
2. **indexbench** (applied): A concurrent hash table with per-bucket locking.
   65,536 buckets, 1M key range, 500K pre-filled keys. Operations: get (80%),
   put (10%), delete (10%). Key distributions: uniform and Zipfian (θ=0.99).
3. **arraybench** (array): A shared array (65,536 uint64 elements) with two locking
   modes: **single** (one global lock) and **striped** (64
   per-stripe locks). Reads scan 16 consecutive elements,
   writes update a single element.

## 3. Results

### 3.1 Raw Lock Throughput (lockbench, mutex workload)

Threads contend on a **single shared lock** with no critical section work (cs_ns=0).

| Lock | 1 thread (Mops/s) | 2 threads | 4 threads | 8 threads |
|------|-------------------|-----------|-----------|-----------|
| TAS | **1,424** | 115 | 39.7 | 8.5 |
| TTAS | 1,370 | 173 | **75.9** | **23.0** |
| CAS | 983 | **192** | 75.3 | 23.7 |
| Ticket | 709 | 26.5 | 13.1 | 5.6 |
| RW (write) | 1,049 | 145 | 45.5 | 8.9 |
| OCC (write) | 356 | 121 | 58.3 | 10.4 |

#### Observations

- **TAS**  degrades rapidly
  under contention because every spin iteration issues an RMW, bouncing the cache line
  between cores (8 threads: 8.5 Mops/s, **167× drop**).
- **TTAS and CAS** are nearly identical multi-threaded. Their spin-read phase (`ldrb`)
  keeps the line in shared state, issuing the RMW only when the lock appears free.
  TTAS is slightly faster single-threaded since `swpab` is simpler than `casab`.
- **Ticket lock** is 2–5× lower throughput but achieves **near-perfect fairness**
  (ratio ≥ 0.999). Its `lock()` touches two cache lines (`ldadd` on `next`,
  `ldapur` on `owner` at +64B), and FIFO ordering prevents lock stealing.
- **RW and OCC** in exclusive mode have higher overhead from wider atomics
  (`casa` on 32/64-bit words vs. byte-sized operations).

#### Fairness (4 threads, cs_ns=0)

| Lock | Min ops | Max ops | Ratio |
|------|---------|---------|-------|
| TAS | 29.0M | 30.7M | 0.945 |
| TTAS | 53.6M | 67.0M | 0.800 |
| CAS | 47.5M | 63.0M | 0.755 |
| Ticket | 9.9M | 9.9M | **1.000** |
| RW | 30.8M | 33.5M | 0.920 |
| OCC | 37.6M | 50.3M | 0.747 |

TTAS/CAS achieve higher throughput by being unfair: a thread that just released
the lock reacquires it before others see the release (hot L1 cache line). Ticket
lock enforces FIFO at the cost of ~83% lower throughput.

---

### 3.2 Critical Section Cost Sweep (4 threads)

| cs_ns | TAS (ns/op) | TTAS | CAS | Ticket |
|-------|-------------|------|-----|--------|
| 0 | 25 | 13 | 13 | 75 |
| 50 | 96 | 96 | 95 | 117 |
| 100 | 136 | 115 | 116 | 142 |
| 500 | 435 | 408 | 408 | 432 |
| 1,000 | 810 | 783 | 784 | 812 |
| 5,000 | 3,817 | 3,788 | 3,789 | 3,830 |

All locks converge by cs_ns=500. The bottleneck shifts to the critical section,
and lock overhead becomes negligible. The choice of lock only matters under
low-cs workloads.

**Fairness diverges significantly at cs_ns=100:**

| Lock | Min thread ops | Max thread ops | Ratio |
|------|----------------|----------------|-------|
| TAS | 5.4M | 5.6M | 0.955 |
| TTAS | 4.5M | 10.6M | 0.428 |
| CAS | 4.0M | 7.7M | 0.521 |
| Ticket | 5.3M | 5.3M | **1.000** |

With a meaningful critical section, TTAS and CAS show severe starvation. The releasing thread keeps the line hot in its L1 and reacquires before waiters can observe the release. 

---

### 3.3 Reader-Writer Workloads (RW and OCC)

| Lock | Read % | 1 thread (Mops/s) | 2 threads | 4 threads | 8 threads |
|------|--------|-------------------|-----------|-----------|-----------|
| RW | 50% | 78 | 25 | 12 | 4.3 |
| RW | 80% | 105 | 32 | 15 | 4.3 |
| RW | 95% | 125 | 29 | 18 | 4.2 |
| OCC | 50% | 75 | 26 | 14 | 4.0 |
| OCC | 80% | 104 | 43 | **35** | **10.3** |
| OCC | 95% | 169 | **76** | **78** | **36** |
| RCU | 50% | 55 | 16 | 11 | 3.7 |
| RCU | 80% | 84 | 32 | 20 | 7.1 |
| RCU | 95% | 118 | 87 | 76 | 24 |

#### Observations

- **RW lock does not scale with read percentage.** At 8 threads, throughput is
  ~4.2–4.3 Mops/s regardless of read ratio. Each `read_lock` issues a `casa` RMW
  and each `read_unlock` issues `ldaddl` — two cache-line invalidations per read.
  Readers serialize on the same cache line as writers.
- **OCC scales dramatically at high read ratios.** At 95% reads, 4 threads: OCC
  delivers 78 Mops/s vs. RW's 18 Mops/s (**4.3×**). Optimistic reads are pure
  loads (`ldapr` + `dmb ishld` + `ldr`) — no RMW, no cache-line invalidation.
- **RCU** provides strong read scaling at 95% (76 Mops/s at 4T, matching OCC),
  but the write-side `synchronize()` must drain all reader epochs => expensive for
  write-heavy workloads.

---

### 3.4 Concurrent Hash Index (indexbench)

#### Uniform Key Distribution (low contention per bucket)

65,536 buckets, 1M key range → ~15 keys/bucket on average.
80% get / 10% put / 10% delete.

| Lock | 1 thread (Mops/s) | 2 threads | 4 threads | 8 threads | Speedup (1→8) |
|------|-------------------|-----------|-----------|-----------|---------------|
| TAS | 30.7 | 63.2 | 121.8 | **146.0** | 4.75× |
| TTAS | 29.7 | 59.0 | 121.6 | 143.0 | 4.81× |
| CAS | 27.4 | 58.2 | 118.9 | 145.6 | 5.31× |
| Ticket | 21.3 | 43.2 | 85.8 | 117.0 | 5.49× |
| RW | 27.5 | 58.6 | 118.0 | 143.5 | 5.22× |
| OCC | 24.6 | 52.9 | 105.1 | 144.1 | **5.86×** |

#### Analysis

- **TAS, TTAS, CAS** are nearly indistinguishable. With 65K buckets and uniform keys,
  per-bucket contention is negligible, so the spin-read optimization of TTAS/CAS
  provides no benefit. Lock overhead (~3–5 ns) is dwarfed by hash computation and
  bucket traversal (~30–40 ns/op single-threaded).
- **All three scale nearly linearly** to 4 threads (~4×) and well to 8 threads
  (~4.7–5.3×).
- **Ticket lock** has ~1.4× higher per-op overhead single-threaded (the two-counter
  protocol). Its scaling ratio (5.49×) is better than TAS (4.75×) because FIFO
  ordering reduces cache-line pingponging at higher thread counts.
- **OCC** has the highest single-threaded overhead (version read + validate) but the
  best scaling (5.86×). Optimistic reads don't invalidate the bucket's cache line,
  which helps at 8 threads even with uniform access.
- **RW lock** performs comparably to TAS/TTAS — the CAS cost in `read_lock()`
  eliminates the theoretical benefit of shared reads at this contention level.

#### Zipfian Key Distribution (θ=0.99, high contention on hot buckets)

| Lock | 1 thread (Mops/s) | 2 threads | 4 threads | 8 threads | Speedup (1→8) |
|------|-------------------|-----------|-----------|-----------|---------------|
| TAS | 24.5 | 48.5 | 88.8 | 100.7 | 4.11× |
| TTAS | 24.3 | 42.5 | 78.8 | 81.5 | 3.36× |
| CAS | 18.7 | 43.2 | 67.4 | 71.9 | 3.84× |
| Ticket | 17.1 | 38.4 | 74.4 | 87.9 | 5.14× |
| RW | 21.8 | 43.1 | 83.8 | 99.5 | 4.56× |
| OCC | 20.6 | 39.3 | **86.3** | **117.3** | **5.69×** |

**OCC wins decisively under Zipfian.** At 8 threads, OCC achieves 117 Mops/s vs.
TAS's 101 Mops/s (**1.16×**). Hot buckets are accessed mostly for reads (80%), and
OCC's optimistic reads avoid cache-line invalidation on contested buckets — the exact
access pattern of B-tree root and upper-level nodes.

Under skew, TTAS/CAS scaling degrades more than TAS (3.4× vs. 4.1×): spin-read
threads spinning on hot lock bytes still add coherence traffic on the hot bucket's
cache line.

---

### 3.5 Contention Density (indexbench, 4 threads, uniform)

Varying bucket count controls contention density (keys/bucket).

| Buckets | Keys/Bucket | TTAS (Mops/s) | CAS | Ticket | OCC |
|---------|-------------|---------------|-----|--------|-----|
| 64 | 15,625 | 0.41 | 0.40 | 0.41 | 0.37 |
| 256 | 3,906 | 1.34 | 1.36 | 1.28 | 1.34 |
| 1,024 | 977 | 3.87 | 4.05 | 4.01 | 3.53 |
| 4,096 | 244 | 13.8 | 16.2 | 14.5 | 13.8 |
| 16,384 | 61 | 49.6 | 48.0 | 41.2 | 44.8 |
| 65,536 | 15 | **98.2** | 98.2 | 71.2 | 85.1 |
| 262,144 | 4 | **130.5** | 117.0 | 95.9 | 114.5 |

#### Analysis

- At **high contention** (64–1,024 buckets), all locks converge — threads spend most
  time waiting while traversing long bucket chains (thousands of entries per bucket).
  Lock algorithm choice is irrelevant here.
- At **medium contention** (4K–16K buckets), CAS slightly leads; the difference
  is within run-to-run variance.
- At **low contention** (65K+ buckets), TTAS consistently leads. Throughput continues
  growing through 262K buckets (130.5 Mops/s) — the working set (262K × 128+ bytes ≈
  32 MB) begins exceeding L2 but prefetching still helps TTAS's sequential spin-read.
- **Ticket lock** scales with decreasing contention but never reaches TTAS/CAS peak
  throughput due to the two-cache-line acquire protocol.

---

### 3.6 Shared Array Benchmark (arraybench)

#### Single Lock — 80% Read / 20% Write

All threads contend on **one global lock** protecting a 65,536-element array.
Reads scan 16 consecutive elements; writes update one element.

| Lock | 1 thread (Mops/s) | 2 threads | 4 threads | 8 threads | Speedup (1→8) |
|------|-------------------|-----------|-----------|-----------|---------------|
| TAS | 62.8 | 32.9 | 17.4 | 4.5 | 0.07× |
| TTAS | 61.7 | 45.3 | 27.8 | 8.5 | 0.14× |
| CAS | 61.9 | 45.7 | 25.7 | 8.2 | 0.13× |
| Ticket | 58.3 | 18.3 | 11.8 | 5.9 | 0.10× |
| RW | 61.2 | 38.5 | 21.5 | 5.5 | 0.09× |
| OCC | **62.2** | **79.6** | **61.8** | **17.1** | **0.27×** |

#### Observations

- **All exclusive locks scale negatively** with a single global lock — threads
  serialize completely and adding threads only increases contention overhead.
- **OCC is the only primitive that scales beyond 1 thread ** because 80% of operations are lock-free optimistic reads.
- **TTAS and CAS** are the best exclusive spinlocks, consistent with the raw
  lock benchmarks. Their spin-read phase reduces cache-line bouncing under contention.
- **Ticket lock** drops sharply at 2 threads (18.3 vs. TTAS's 45.3 Mops/s) due to
  the `ldadd` on `next` bouncing under high contention, but maintains perfect fairness.
- **RW lock** performs worse than TTAS/CAS despite supporting shared reads: the
  `casa` RMW in `read_lock()` serializes readers on the same contended cache line.

#### Single Lock — 95% Read 

| Lock | 1 thread (Mops/s) | 2 threads | 4 threads | 8 threads |
|------|-------------------|-----------|-----------|-----------|
| TAS | 65.8 | 33.6 | 18.0 | 5.0 |
| TTAS | 64.1 | 46.0 | 30.0 | 9.3 |
| CAS | 64.1 | 47.0 | 27.5 | 8.8 |
| Ticket | 64.2 | 17.4 | 11.9 | 5.9 |
| RW | 62.7 | 37.4 | 24.7 | 5.7 |
| OCC | 64.7 | **106.9** | **136.3** | **80.0** |

**OCC dominates at high read ratios.** At 4 threads with 95% reads, OCC delivers
136 Mops/s (2.1× its own single-threaded rate and 4.5× faster than TTAS
(30.0 Mops/s)). With only 5% writes, validation almost never fails, so readers execute
fully in parallel with zero cache-line invalidations.

#### Single Lock — 50% Read

| Lock | 1 thread (Mops/s) | 4 threads | 8 threads |
|------|-------------------|-----------|-----------|
| TAS | 62.2 | 15.8 | 4.2 |
| TTAS | 62.1 | 25.6 | 8.2 |
| CAS | 62.4 | 23.7 | 7.2 |
| Ticket | 62.0 | 11.8 | 5.8 |
| RW | 62.0 | 18.9 | 5.0 |
| OCC | 60.7 | 25.8 | 7.2 |

At 50% writes, OCC's advantage largely disappears, frequent writes cause optimistic
reads to fail validation and retry. 

#### Striped Locks (64 Stripes) — 80% Read

64 independent locks, each protecting a 1,024-element stripe.

| Lock | 1 thread (Mops/s) | 2 threads | 4 threads | 8 threads | Speedup (1→8) |
|------|-------------------|-----------|-----------|-----------|---------------|
| TAS | 54.0 | 87.6 | **145.1** | 104.7 | 1.94× |
| TTAS | 54.4 | 86.4 | **145.0** | 105.9 | 1.95× |
| CAS | 53.8 | 86.1 | 142.3 | 105.2 | 1.95× |
| Ticket | 53.5 | 72.9 | 111.9 | 97.2 | 1.82× |
| RW | 52.4 | 79.2 | 142.4 | 104.0 | 1.98× |
| OCC | 53.5 | 84.3 | 140.6 | 102.7 | 1.92× |

#### Observations

- **Striped locks restore positive scaling for all primitives.** This confirms that
  contention partitioning is the primary enabler of parallelism, not the lock
  algorithm itself.
- **All locks peak at 4 threads** and regress at 8. With 8 threads scanning the
  512 KB array (65K × 8 bytes) => memory bandwidth not lock contention becomes
  the bottleneck.
- **TAS, TTAS, and CAS** lead at 4 threads (~145 Mops/s) with near-identical results.
  At low per-stripe contention, TTAS's spin-read phase provides no advantage.
- **OCC's advantage disappears** in the striped case, exclusive lock contention is
  already low, so the optimistic read path's benefit is marginal and version-check
  overhead slightly hurts.
- **Ticket lock** has slightly lower peak (112 Mops/s at 4T) but the best 8-thread
  relative performance (97.2 vs. ~104 Mops/s for TAS/TTAS), consistent with its FIFO
  ordering reducing cache-line thrashing at higher thread counts.

#### Single Lock vs. Striped

| Lock | Single 4T (Mops/s) | Striped 4T (Mops/s) | Improvement |
|------|--------------------|--------------------|-------------|
| TTAS | 27.8 | 145.0 | **5.2×** |
| OCC | 61.8 | 140.6 | 2.3× |
| Ticket | 11.8 | 111.9 | **9.5×** |

Striping provides **5–10× improvement** for exclusive locks. OCC benefits less because
it already avoids read-path contention. Reducing contention density matters more than
optimizing the lock algorithm.

---

### 3.7 Generated Assembly Analysis (ARM64, Apple M3 Pro)

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
single-threaded (983 vs. 1,370 Mops/s) — `casab` needs both an expected and
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

`lock()` accesses two different cache lines `ldadd`
on `next` at `[x0]` and `ldapur` on `owner` at `[x0, #64]`. The `alignas(64)` in the
C++ code ensures these counters live on separate 64-byte cache lines, avoiding false
sharing between the ticket dispenser and the "now serving" display.

Second, the spin loop only reads `owner` (`ldapur`), it never writes. Only the
unlock path writes to `owner`, and since exactly one thread holds the lock, there
is only one writer to the owner cache line at any time. This is why the spin
loop generates less coherence traffic than TAS. However, the `ldadd` on `next`
during lock acquisition is an RMW that bounces under heavy contention, which is
why ticket lock is still slower than TTAS overall.

The unlock uses `stlur` (store-release with unscaled offset), a non-atomic store
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
per read. OCC causes **zero invalidations**, which is why at 95% reads and 4 threads,
OCC achieves 136 Mops/s vs. RW's 24.7 Mops/s (**5.5×**) on the array benchmark.

The tradeoff: validation fails if a writer was active during the read, requiring a
retry. This overhead is measurable once writes exceed ~20–30% of operations.

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

### Key Takeaways

1. **Lock overhead is negligible when contention is low.** With 65K+ buckets and
   uniform access, the lock acquire/release adds ~3–5 ns to a ~30–40 ns operation.
   Data structure design (bucket count, node size, cache locality) matters far more
   than the lock choice.

2. **Under skew, OCC wins.** Zipfian workloads concentrate accesses on hot
   buckets/nodes. OCC readers don't invalidate the cache line, enabling true read
   parallelism — the exact access pattern of B-tree root and upper-level nodes.

3. **Fairness and throughput are in tension.** TTAS/CAS achieve high throughput by
   being unfair (recent releasers reacquire faster), but can starve threads when
   critical sections are non-trivial (min/max ratio ≈ 0.4 at cs_ns=100). Ticket lock
   guarantees FIFO order at ~40–80% throughput cost, depending on contention level.

4. **RW locks disappoint in practice.** The theoretical advantage of shared reads
   is negated by the CAS in `read_lock()`/`read_unlock()`. OCC achieves the same
   goal with zero read-path atomics.

5. **Contention density is the dominant factor.** Lock striping (Section 3.6) yields
   5–10× improvement for exclusive locks. Choosing the right data structure granularity
   provides more benefit than optimizing the lock algorithm.

---

## 5. Reproducing the Experiments

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Raw lock benchmarks
./build/lockbench --lock ttas --workload mutex --threads 4 --seconds 3
./build/lockbench --lock occ --workload rw --threads 4 --seconds 3 --read_pct 95

# Concurrent index benchmarks
./build/indexbench --lock ttas --dist uniform --threads 8 --seconds 5 --read_pct 80 --insert_pct 10
./build/indexbench --lock occ --dist zipfian --threads 8 --seconds 5 --read_pct 80 --insert_pct 10

# Shared array benchmarks
./build/arraybench --lock ttas --mode single --threads 4 --seconds 3 --read_pct 80
./build/arraybench --lock occ --mode single --threads 4 --seconds 3 --read_pct 95
./build/arraybench --lock ttas --mode striped --threads 8 --seconds 3 --stripes 64

# Generate assembly for all locks
./scripts/gen_asm.sh          # outputs to asm/

# Full sweeps
./scripts/sweep.sh 3 8          # Raw lock sweep
./scripts/index_sweep.sh 3 8    # Index sweep
./scripts/array_sweep.sh 3 8    # Array sweep
```

### Available Options

**lockbench**: `--lock tas|ttas|cas|ticket|rw|occ|rcu` `--workload mutex|rw|rcu`
`--threads N` `--seconds S` `--warmup S` `--cs_ns NS` `--read_pct P`

**indexbench**: `--lock tas|ttas|cas|ticket|rw|occ` `--dist uniform|zipfian`
`--threads N` `--seconds S` `--warmup S` `--read_pct P` `--insert_pct P`
`--zipf_theta T` `--buckets N` `--key_range N` `--prefill N`

**arraybench**: `--lock tas|ttas|cas|ticket|rw|occ` `--mode single|striped`
`--threads N` `--seconds S` `--warmup S` `--read_pct P` `--array_size N`
`--stripes N` `--scan_len N`
