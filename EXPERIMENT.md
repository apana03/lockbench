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

- **Warmup**: 1–2 seconds before measurement (excluded from results).
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
| TAS | **1,413** | 112 | 40 | 8.2 |
| TTAS | 1,372 | 171 | **76** | **26** |
| CAS | 988 | **195** | 76 | 26 |
| Ticket | 708 | 38 | 22 | 9.0 |
| RW (write) | 998 | 176 | 51 | 9.1 |
| OCC (write) | 363 | 136 | 59 | 9.4 |

#### Observations

- **TAS** is fastest single-threaded (1.4 GHz). On Apple M3 Pro with ARMv8.1
  LSE, `test_and_set` compiles to a single `swpab` (atomic swap byte with
  acquire) (see Section 3.7 for the full assembly). However, it scales poorly
  because every spin iteration issues an RMW, causing cache-line bouncing.
- **TTAS and CAS** are near-identical multi-threaded. The spin-read phase
  (`ldrb` plain load) keeps RMW traffic low — only issuing `swpab`/`casab`
  when the lock looks free. TTAS slightly wins at 1 thread because `swpab`
  is simpler than `casab` (no comparison step).
- **Ticket lock** is 2–3× slower in throughput but provides **near-perfect
  fairness** (ratio ≥ 0.999 at all thread counts). The `lock()` path touches
  two separate cache lines (`ldadd` on `next`, then `ldapur` on `owner` at
  offset +64), and the FIFO spin prevents lock stealing.
- **RW and OCC** in exclusive mode have overhead from their more complex state
  encoding (`casa` on a 32/64-bit word vs. byte-sized flag operations).

#### Fairness (4 threads, cs_ns=0)

| Lock | Min ops | Max ops | Ratio |
|------|---------|---------|-------|
| TAS | 29.6M | 30.2M | 0.982 |
| TTAS | 48.2M | 64.1M | 0.752 |
| CAS | 46.5M | 68.7M | 0.676 |
| Ticket | 16.6M | 16.6M | **1.000** |
| RW | 36.2M | 39.6M | 0.916 |
| OCC | 39.7M | 48.9M | 0.813 |

**Key insight**: TTAS/CAS achieve higher throughput by being *unfair*. A thread
that just released the lock is likely to reacquire it immediately (cache-line is
hot in its L1). Ticket lock prevents this but pays a throughput penalty.

---

### 3.2 Critical Section Cost Sweep (lockbench, 4 threads)

How do locks behave as the work inside the critical section grows?

| cs_ns | TAS (ns/op) | TTAS | CAS | Ticket |
|-------|-------------|------|-----|--------|
| 0 | 25 | 13 | 13 | 45 |
| 50 | 93 | 92 | 92 | 116 |
| 100 | 124 | 114 | 114 | 141 |
| 500 | 433 | 407 | 407 | 432 |
| 1,000 | 810 | 783 | 783 | 809 |
| 5,000 | 3,814 | 3,786 | 3,786 | 3,822 |

As cs_ns grows, lock overhead becomes negligible — all primitives converge
because the bottleneck shifts from lock acquisition to the critical section work.
At cs_ns=500+ the choice of lock barely matters for throughput.

**However, fairness diverges dramatically.** At cs_ns=100:

| Lock | Min thread ops | Max thread ops | Ratio |
|------|---------------|----------------|-------|
| TAS | 5.90M | 6.18M | 0.955 |
| TTAS | **1** | 10.0M | **~0** |
| CAS | 1.13M | 9.95M | 0.114 |
| Ticket | 5.33M | 5.33M | **1.000** |

TTAS under load with meaningful critical sections can **completely starve threads**.
One thread reacquires the lock before others even see it released. Ticket lock
maintains perfect fairness regardless of critical section length.

---

### 3.3 Reader-Writer Workloads (lockbench, RW and OCC)

| Lock | Read % | 1 thread (Mops/s) | 2 threads | 4 threads | 8 threads |
|------|--------|-------------------|-----------|-----------|-----------|
| RW | 50% | 82 | 26 | 12 | 4.5 |
| RW | 80% | 141 | 33 | 15 | 4.6 |
| RW | 95% | 174 | 37 | 18 | 4.6 |
| OCC | 50% | 78 | 28 | 14 | 4.0 |
| OCC | 80% | 135 | 46 | **36** | **8.8** |
| OCC | 95% | 132 | **81** | **82** | **51** |
| RCU | 50% | 46 | 16 | 10 | 3.4 |
| RCU | 80% | 73 | 29 | 20 | 7.0 |
| RCU | 95% | 151 | 76 | 77 | 24 |

#### Observations

- **RW lock does not scale with read percentage.** At 8 threads, throughput is
  ~4.6 Mops/s regardless of read ratio. The `fetch_sub` in `read_unlock()` still
  bounces the cache line, and the CAS in `read_lock()` serializes under contention.
- **OCC scales dramatically at high read ratios.** At 95% reads, 4 threads:
  OCC delivers 82 Mops/s vs. RW's 18 Mops/s (**4.6×**). Optimistic reads
  involve only loads — no cache-line invalidation. This is the key advantage
  for read-heavy indexes.
- **RCU** provides good read scaling (77 Mops/s at 95% reads, 4 threads) but
  the write-side `synchronize()` is expensive — it must drain all reader epochs.
  RCU is best when writes are very rare.

---

### 3.4 Concurrent Hash Index (indexbench)

#### Uniform Key Distribution (low contention per bucket)

65,536 buckets, 1M key range → ~15 keys/bucket on average.
80% get / 10% put / 10% delete.

| Lock | 1 thread (Mops/s) | 2 threads | 4 threads | 8 threads | Speedup (1→8) |
|------|-------------------|-----------|-----------|-----------|---------------|
| TAS | 46.0 | 95.7 | 192.8 | **228.6** | **4.97×** |
| TTAS | 47.9 | 95.0 | 193.5 | 228.1 | 4.76× |
| CAS | 46.9 | 93.3 | 190.5 | 225.7 | 4.81× |
| Ticket | 28.6 | 57.6 | 113.8 | 161.4 | 5.64× |
| RW | 46.0 | 91.4 | 188.5 | 219.2 | 4.77× |
| OCC | 37.0 | 74.9 | 154.4 | 217.2 | 5.87× |

#### Analysis

- **TAS, TTAS, CAS** are nearly indistinguishable in the index context. With
  65K buckets and uniform keys, contention per bucket is negligible, so the
  spin-read optimization of TTAS/CAS provides no benefit. Lock overhead is
  dwarfed by hash computation + bucket traversal (~18–21 ns/op single-threaded).
- **All three achieve near-linear scaling** to 4 threads (4.1×) and good scaling
  to 8 threads (4.8–5.0×). The 11-core M3 Pro has limited bandwidth for 8
  threads of index operations.
- **Ticket lock has 1.6× higher per-operation overhead** single-threaded (35 ns
  vs. 21 ns) due to the two-counter protocol. But its scaling ratio is actually
  better (5.64×) because the FIFO ordering prevents cache-line pingponging.
- **OCC** has the highest single-threaded overhead (27 ns — version
  read+validate), but the **best scaling** (5.87×). At 8 threads, it nearly
  catches up to TAS/TTAS because optimistic reads don't invalidate the bucket's
  cache line.
- **RW lock** with per-bucket shared reads performs well but doesn't beat the
  simple spinlocks — the CAS cost in `read_lock()`/`read_unlock()` is comparable
  to exclusive acquire/release at this contention level.

#### Zipfian Key Distribution (θ=0.99, high contention on hot buckets)

| Lock | 1 thread (Mops/s) | 2 threads | 4 threads | 8 threads | Speedup (1→8) |
|------|-------------------|-----------|-----------|-----------|---------------|
| TAS | 32.2 | 60.3 | 112.8 | 122.3 | 3.80× |
| TTAS | 32.4 | 60.7 | 113.3 | 127.4 | 3.93× |
| CAS | 31.7 | 59.8 | 112.5 | 125.9 | 3.97× |
| Ticket | 28.4 | 52.6 | 96.3 | 107.8 | 3.80× |
| RW | 31.1 | 58.5 | 110.6 | 123.7 | 3.98× |
| OCC | 30.7 | 59.6 | **115.2** | **154.0** | **5.02×** |

**OCC wins decisively under Zipfian.** At 8 threads, OCC achieves 154 Mops/s
vs. TTAS's 127 Mops/s (**1.21×**). Hot buckets are accessed mostly for reads
(80%), and OCC's optimistic reads avoid cache-line invalidation on those
contested buckets. This is the exact access pattern of real-world indexes (e.g.,
B-tree root and upper-level nodes are hot but rarely written).

**Ticket lock** still provides the best fairness but at lower throughput. Under
Zipfian, its gap narrows because all locks suffer from hot-bucket serialization.

---

### 3.5 Contention Density (indexbench, 4 threads, uniform)

Varying bucket count controls how many keys map to each bucket (contention density).

| Buckets | Keys/Bucket | TTAS (Mops/s) | CAS | Ticket | OCC |
|---------|-------------|---------------|-----|--------|-----|
| 64 | 15,625 | 0.54 | 0.34 | 0.46 | 0.55 |
| 256 | 3,906 | 1.56 | 1.59 | 1.85 | 1.71 |
| 1,024 | 977 | 5.13 | 3.57 | 5.04 | 5.73 |
| 4,096 | 244 | 19.7 | 13.8 | 15.4 | 16.4 |
| 16,384 | 61 | 66.4 | 44.9 | 46.3 | 58.5 |
| 65,536 | 15 | **159.2** | 133.1 | 92.7 | 139.4 |
| 262,144 | 4 | 127.9 | **143.5** | 104.9 | 135.9 |

#### Analysis

- At **high contention** (64–1024 buckets), all locks converge because threads
  spend most of their time waiting. Critical section length (bucket traversal
  with thousands of entries) dominates lock overhead.
- At **medium contention** (4K–16K buckets), TTAS pulls ahead due to its
  efficient spin-read phase. OCC is competitive here.
- At **low contention** (65K+ buckets), the picture changes: TTAS peaks at 65K
  buckets then drops at 262K. This is a **cache capacity effect** — 262K buckets
  × 128+ bytes/bucket exceeds L2 cache, causing misses on bucket access. CAS
  actually wins at 262K buckets, likely because its slightly different memory
  access pattern interacts better with the prefetcher.
- **Ticket lock** scales linearly with decreasing contention but never reaches
  the peak throughput of TTAS/CAS due to its inherent two-cache-line protocol.

---

### 3.6 Shared Array Benchmark (arraybench)

#### Single Lock — 80% Read / 20% Write

All threads contend on **one global lock** protecting a 65,536-element array.
Reads scan 16 consecutive elements; writes update one element.

| Lock | 1 thread (Mops/s) | 2 threads | 4 threads | 8 threads | Speedup (1→8) |
|------|-------------------|-----------|-----------|-----------|---------------|
| TAS | 71.7 | 37.9 | 19.8 | 4.6 | 0.06× |
| TTAS | 70.1 | 50.4 | 28.8 | 8.7 | 0.12× |
| CAS | 65.1 | 49.3 | 25.2 | 8.1 | 0.12× |
| Ticket | 67.6 | 18.4 | 10.9 | 5.3 | 0.08× |
| RW | 62.3 | 40.7 | 20.8 | 5.7 | 0.09× |
| OCC | **65.3** | **83.3** | **60.9** | **17.5** | **0.27×** |

#### Observations

- **All exclusive locks show negative scaling** with a single global lock. This is
  expected: the entire array is one critical section, so threads serialize completely.
  Adding threads only adds contention overhead.
- **OCC is the only primitive that scales beyond 1 thread.** At 2 threads, OCC
  achieves 83.3 Mops/s vs. its own 65.3 Mops/s single-threaded (1.28×). At 4
  threads it reaches 60.9 Mops/s — still close to single-threaded. This is because
  80% of operations are optimistic reads that don't acquire any lock at all.
- **TTAS and CAS** are the best exclusive spinlocks, following the same pattern as
  the raw lock benchmarks. Their spin-read phase reduces cache-line bouncing.
- **Ticket lock** pays a heavy penalty under high contention (18.4 Mops/s at 2
  threads vs. TTAS's 50.4 Mops/s) but maintains near-perfect fairness (ratio ≥ 0.999).
- **RW lock** performs worse than TTAS/CAS despite supporting shared reads. The CAS
  overhead in `read_lock()` on a single contended cache line negates the benefit.

#### Single Lock — 95% Read (Read-Heavy)

| Lock | 1 thread (Mops/s) | 2 threads | 4 threads | 8 threads |
|------|-------------------|-----------|-----------|-----------|
| TAS | 74.5 | 39.0 | 19.4 | 5.3 |
| TTAS | 74.1 | 53.0 | 34.6 | 9.4 |
| CAS | 74.0 | 54.2 | 31.2 | 8.9 |
| Ticket | 74.1 | 20.2 | 13.7 | 6.4 |
| RW | 72.6 | 45.4 | 27.7 | 5.8 |
| OCC | 73.8 | **121.5** | **155.1** | **82.1** |

**OCC dominates at high read ratios.** At 4 threads with 95% reads, OCC delivers
155 Mops/s — more than **double** its single-threaded throughput and **4.5× faster**
than the next best lock (TTAS at 34.6 Mops/s). With only 5% writes, optimistic read
validation almost never fails, so readers execute fully in parallel.

All exclusive locks remain below their single-threaded performance regardless of read
percentage, because every operation (read or write) acquires the global lock.

#### Single Lock — 50% Read (Write-Heavy)

| Lock | 1 thread (Mops/s) | 4 threads | 8 threads |
|------|-------------------|-----------|-----------|
| TAS | 63.7 | 13.7 | 4.2 |
| TTAS | 62.8 | 22.5 | 8.0 |
| CAS | 64.1 | 19.9 | 7.2 |
| Ticket | 63.7 | 11.1 | 5.9 |
| RW | 63.7 | 19.5 | 5.1 |
| OCC | 63.2 | 27.2 | 6.8 |

Under heavy writes, OCC's advantage shrinks because optimistic reads frequently fail
validation (50% of concurrent operations are writes). At 8 threads, TTAS slightly
edges OCC (8.0 vs. 6.8 Mops/s) — failed optimistic reads must retry, adding wasted
work. The lesson: **OCC only helps when the read-to-write ratio is high.**

#### Striped Locks (64 Stripes) — 80% Read

64 independent locks, each protecting a 1,024-element stripe. Contention per stripe
is 64× lower than the single-lock case.

| Lock | 1 thread (Mops/s) | 2 threads | 4 threads | 8 threads | Speedup (1→8) |
|------|-------------------|-----------|-----------|-----------|---------------|
| TAS | 62.1 | 99.5 | 159.8 | 117.4 | 1.89× |
| TTAS | 61.3 | 98.7 | 159.2 | 118.2 | 1.93× |
| CAS | 59.6 | 87.7 | 148.4 | 91.1 | 1.53× |
| Ticket | 49.0 | 76.4 | 115.3 | 95.9 | 1.96× |
| RW | 58.1 | 92.1 | 143.3 | 118.6 | 2.04× |
| OCC | 51.5 | 90.7 | 154.9 | 114.2 | 2.22× |

#### Observations

- **Striped locks restore positive scaling.** All locks now scale to at least 4
  threads, confirming that partitioned contention is the primary enabler of
  parallelism — not the lock algorithm.
- **All locks peak at 4 threads** and plateau or regress at 8 threads. This is a
  **memory bandwidth bottleneck**: 8 threads scanning array elements saturate the
  memory subsystem (65K × 8 bytes = 512 KB, fitting in L2 but with 8 threads
  competing for L2/L3 bandwidth).
- **TAS and TTAS** lead at 4 threads (~160 Mops/s) with near-identical performance.
  At this contention level (uniform access over 64 stripes with 4 threads), stripe
  collisions are rare, so the spin-read optimization of TTAS provides no advantage.
- **OCC's advantage disappears** compared to the single-lock case. With 64 stripes,
  exclusive lock contention is already low, so the optimistic read path's benefit
  is marginal. OCC's extra version-read overhead actually makes it slightly slower
  than TAS/TTAS in the striped configuration.
- **Ticket lock** has the lowest single-threaded throughput (49 Mops/s vs. 62 Mops/s
  for TAS) due to its two-counter protocol, but achieves the best scaling ratio
  alongside OCC, consistent with its FIFO fairness preventing cache-line thrashing.

#### Key Insight: Single Lock vs. Striped

| Lock | Single 4T (Mops/s) | Striped 4T (Mops/s) | Improvement |
|------|--------------------|--------------------|-------------|
| TTAS | 28.8 | 159.2 | **5.5×** |
| OCC | 60.9 | 154.9 | 2.5× |
| Ticket | 10.9 | 115.3 | **10.6×** |

Partitioning contention via striping provides **5–10× improvement** for exclusive
locks. OCC benefits less from striping because it already avoids contention on the
read path. This reinforces the conclusion from Section 3.5: **reducing contention
density matters more than optimizing the lock implementation.**

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

TAS is the simplest lock: 3 instructions on the fast path (mov + swpab + tbz).
The problem is the spin loop — every iteration does a `swpab`, which is an RMW
that takes exclusive ownership of the cache line. Under contention, every core
is hammering the same line with store operations, causing it to bounce between
L1 caches (MOESI protocol transitions). This is why TAS throughput drops from
1,413 Mops/s at 1 thread to just 8.2 Mops/s at 8 threads.

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
traffic under contention — the benchmarks show TTAS sustaining 26 Mops/s at 8
threads vs. TAS's 8.2 Mops/s (3.2× better).

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
single-threaded (988 vs. 1,372 Mops/s) — `casab` needs both an expected and
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

Two things stand out. First, `lock()` accesses **two different cache lines**: `ldadd`
on `next` at `[x0]` and `ldapur` on `owner` at `[x0, #64]`. The `alignas(64)` in the
C++ code ensures these counters live on separate 64-byte cache lines, avoiding false
sharing between the ticket dispenser and the "now serving" display.

Second, the spin loop only reads `owner` (`ldapur`) — it never writes. Only the
unlock path writes to `owner`, and since exactly one thread holds the lock, there
is only **one writer** to the owner cache line at any time. This is why the spin
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

- `read_lock` requires a `casa` (CAS word, acquire) — a full RMW operation that
  takes exclusive ownership of the cache line. Even though multiple readers can
  hold the lock concurrently, each `read_lock` call **invalidates the cache line
  for all other cores** while the CAS executes. Under contention, readers
  serialize on the CAS.
- `read_unlock` uses `ldaddl` (atomic fetch_add with release) — another RMW.
  So a complete read-side critical section requires **two RMW operations**
  (`casa` + `ldaddl`), each causing a cache-line invalidation.
- `write_lock` does NOT use a spin-read optimization — it issues `casa` on every
  retry in the spin loop. This makes the writer path even more expensive under
  contention compared to TTAS/CAS.

This is 4 instructions of atomic overhead per read operation, compared to OCC's
zero atomics on the read path (see below).

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
- `read_begin`: one `ldapr` (load-acquire) — a plain load with acquire semantics.
  On ARM, `ldapr` does not invalidate the cache line and can be served from shared
  L1 cache state. **No RMW, no cache-line bouncing.**
- `read_validate`: one `dmb ishld` (load fence) + one `ldr` (plain load) + compare.
  Again, **no RMW operations**. The `dmb ishld` is a lightweight load-only barrier
  that prevents the CPU from reordering the preceding data reads past the version
  check.

Compare this to the RW lock's read path: `casa` (RMW) + `ldaddl` (RMW) = two
cache-line invalidations per read. OCC's read path causes **zero invalidations**,
which is why at 95% reads and 4 threads, OCC achieves 155 Mops/s vs. RW's 27.7
Mops/s (5.6×) on the array benchmark.

The tradeoff: if a writer was active during the read, validation fails and the
reader must retry. This only matters when writes are frequent (50%+ write ratio).

#### Instruction Count Summary

| Lock | lock() fast path | unlock() | Read-path atomics |
|------|-----------------|----------|-------------------|
| TAS | 3 insns (`mov` + `swpab` + `tbz`) | 2 insns (`stlrb` + `ret`) | N/A |
| TTAS | 5 insns (`mov` + `ldrb` + `tbnz` + `swpab` + `tbnz`) | 2 insns | N/A |
| CAS | 6 insns (`mov` + `mov` + `casab` + `cmp` + `b.ne` + `ret`) | 2 insns | N/A |
| Ticket | 6 insns (`mov` + `ldadd` + `ldapur` + `cmp` + `b.eq` + `ret`) | 4 insns | N/A |
| RW | read: 7 insns (with `casa`), unlock: `ldaddl` | write: `stlr` | **2 RMW** (`casa` + `ldaddl`) |
| OCC | read: `ldapr` + `tbz`, validate: `dmb` + `ldr` + `cmp` + `cset` | write: `ldaddl` | **0 RMW** |

#### Key Observations from Assembly

1. **ARM LSE eliminates LL/SC overhead.** On older ARM cores without LSE,
   `atomic_flag::test_and_set` would compile to an `ldxrb`/`stxrb` loop (4+
   instructions, with spurious failures from the exclusive monitor). With LSE,
   it becomes a single `swpab`. This is why all our locks are quite fast even
   single-threaded — the M3 Pro's LSE implementation is very efficient.

2. **The TTAS optimization is visible in assembly.** TAS spins with `swpab`
   (RMW every iteration), while TTAS spins with `ldrb` (plain load) and only
   issues `swpab` when the lock looks free. CAS does the same with `ldrb` +
   `casab`. This is the source of the 3× throughput difference at 8 threads.

3. **All unlocks are trivial.** TAS/TTAS/CAS use `stlrb` (store-release byte),
   RW write uses `stlr` (store-release word), ticket uses `stlur` (store-release
   with offset). These are simple stores with release semantics — no RMW needed
   because the lock holder is the only writer.

4. **OCC's read path is uniquely cheap.** `ldapr` + `dmb ishld` + `ldr` — all
   loads, no stores, no RMW. This is fundamentally different from every other
   lock where even reading requires modifying shared state. This matches the
   benchmark results where OCC scales linearly with readers while all other
   locks plateau.

5. **Ticket lock's two-cache-line design is visible.** `[x0]` for `next` and
   `[x0, #64]` for `owner` — the 64-byte offset directly corresponds to the
   `alignas(64)` in the C++ struct. This prevents false sharing but means
   `lock()` touches two cache lines instead of one.

6. **RW write_lock lacks spin-read optimization.** It issues `casa` on every
   spin iteration (like TAS's `swpab`), while OCC's `write_lock` uses a
   TTAS-style `ldr` + `tbnz` spin before the CAS. This makes RW write
   acquisition more expensive under contention.

---

### Key Takeaways

1. **Lock overhead is negligible when contention is low.** With 65K+ buckets and
   uniform access, the lock acquire/release adds ~2–5 ns to a ~20 ns operation.
   The choice of lock matters far less than the data structure design (bucket
   count, node size, cache locality).

2. **Under skew, OCC wins.** Zipfian workloads concentrate accesses on a few hot
   buckets/nodes. OCC readers don't invalidate the cache line, enabling true
   read parallelism. This matches the structure of B-trees (hot root/internal
   nodes, cold leaves).

3. **Fairness and throughput are in tension.** TTAS/CAS achieve high throughput by
   being unfair (recent releasers reacquire faster). Ticket lock guarantees FIFO
   but sacrifices 30–40% throughput. Choose based on whether tail latency or
   aggregate throughput matters more.

4. **RW locks disappoint in practice.** The theoretical advantage of shared read
   access is offset by the CAS overhead in `read_lock()`/`read_unlock()`. OCC
   achieves the same goal more efficiently by avoiding atomics on the read path
   entirely.

5. **Contention density is the dominant factor.** Designing the index for low
   contention (more fine-grained nodes, higher fanout, lock striping) provides
   more benefit than optimizing the lock implementation.

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
