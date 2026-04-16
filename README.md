# lockbench

Benchmarks for synchronization primitives across ARM64 and x86_64.

## Primitives

| Lock | Description |
|------|-------------|
| TAS | Test-and-set spinlock (`atomic_flag::test_and_set`) |
| TTAS | Test-and-test-and-set (spin on reads, then try swap) |
| CAS | Compare-and-swap with TTAS-style spin-read |
| Ticket | FIFO-fair ticket lock (two counters on separate cache lines) |
| RW | Spinning reader-writer lock (shared readers, exclusive writer) |
| OCC | Optimistic concurrency / seqlock (lock-free reads with version validation) |
| RCU | Epoch-based read-copy-update (per-thread epoch announcement) |

## Benchmarks

### lockbench

Raw lock/unlock throughput on a **single shared lock**. Threads run a tight loop
of lock → optional busy-work → unlock. Three workload modes:

- **mutex** — all threads use exclusive `lock()`/`unlock()`. Works with every
  primitive. Measures pure lock acquisition overhead under full contention.
- **rw** — threads call `read_lock()` or `write_lock()` based on `--read_pct`.
  Available for RW and OCC locks. Measures how well the lock allows reader
  parallelism.
- **rcu** — readers announce an epoch and read a shared pointer; writers swap
  the pointer and wait for all readers to drain. Measures epoch-based
  read scaling.

### arraybench

Contention on an **array of independent locks**. Each thread picks a random lock
index, acquires it, does optional busy-work, and releases. The number of locks
(`--num_locks`) controls contention density — fewer locks means more collisions.

For exclusive locks (TAS, TTAS, CAS, Ticket), every operation is a plain
`lock()`/`unlock()`. For RW and OCC locks, each operation is randomly chosen as
a read-lock or write-lock based on `--read_pct`.

All locks are cache-line padded (`alignas(64)`) to isolate true contention from
false sharing. There is no underlying data structure — the benchmark measures
pure lock contention patterns across the array.

### indexbench

Concurrent hash table with per-bucket locking. Supports uniform and Zipfian key
distributions. Operations: get, put, delete with configurable ratios.

### locktest

Correctness tests for all lock implementations. Verifies mutual exclusion by
having N threads perform non-atomic increments (read → write) of a shared
counter inside a critical section, then asserting `counter == threads * loops`.
Tests TAS, TTAS, CAS, Ticket via `lock()`/`unlock()`, and RW/OCC via
`write_lock()`/`write_unlock()`. RCU is excluded since it doesn't provide
mutual exclusion over a shared counter.

## How Measurements Work

### Timing Model

All benchmarks use the same measurement pattern: run for a fixed wall-clock
duration and count completed operations, rather than timing individual operations.

```
 barrier  ──►  warmup phase  ──►  measurement phase  ──►  stop
   │              │                    │                      │
   │         threads run but       measuring=true          stop=true
   │         ops not counted       ops are counted         threads join
   │              │                    │                      │
   │              │                 t0=now()              t1=now()
   └── all threads
       synchronized
```

1. **Barrier synchronization.** All worker threads are spawned and wait at a
   `start_barrier` (spin-barrier using `fetch_add` + busy-wait). The main thread
   waits until all workers have arrived, then releases them simultaneously. This
   ensures no thread starts work before the others are ready.

2. **Warmup phase.** Workers run the full benchmark loop (lock/unlock with
   optional `cs_work`) but do not count operations. The warmup lets caches warm
   up, branch predictors train, and OS scheduling stabilize. Default: 1 second.

3. **Measurement phase.** The main thread sets `measuring=true` (relaxed store)
   and records `t0 = steady_clock::now()`. Workers check `measuring` after each
   operation and increment a thread-local counter only when it is true. After
   `--seconds` seconds, the main thread sets `stop=true` and joins all threads,
   then records `t1`.

4. **Aggregation.** Each thread's local count is accumulated into `total_ops`.
   Throughput is `total_ops / (t1 - t0)`, latency is `(t1 - t0) / total_ops`
   in nanoseconds. Per-thread counts are used to compute fairness (min/max ratio).

### Fairness Metric

Fairness is reported as `min_ops / max_ops` across all threads. A ratio of 1.0
means every thread completed the same number of operations. Lower ratios indicate
starvation — some threads were starved of lock acquisitions while others
dominated.

### Critical Section Simulation

Critical section work is simulated via `busy_work(iters)`:

```cpp
inline void busy_work(std::uint64_t iters) {
  for (std::uint64_t i = 0; i < iters; ++i) {
    asm volatile("" ::: "memory");
  }
}
```

The compiler barrier (`asm volatile("" ::: "memory")`) prevents the loop from
being optimized away without executing any real instruction inside it. Each
iteration is roughly 1-2 cycles on ARM64. This avoids timer-based approaches
that would add measurement noise to short critical sections.

### CSV Output

All benchmarks support `--csv <file>` to append one row per run. The header is
auto-written when the file is empty or missing. Columns include lock type,
thread count, parameters, total/read/write ops, throughput (ops/s), latency
(ns/op), and per-thread fairness metrics.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Run

```bash
# raw lock throughput
./build/lockbench --lock ttas --workload mutex --threads 4 --seconds 3

# with synthetic critical section work (100 loop iterations)
./build/lockbench --lock ttas --workload mutex --threads 4 --cs_work 100

# reader-writer workload
./build/lockbench --lock occ --workload rw --threads 4 --read_pct 95

# RCU workload
./build/lockbench --lock rcu --workload rcu --threads 4 --read_pct 95

# lock array (exclusive locks)
./build/arraybench --lock ttas --threads 4 --num_locks 64

# lock array (OCC with 80% read / 20% write per-operation probability)
./build/arraybench --lock occ --threads 4 --num_locks 64 --read_pct 80

# concurrent hash index (uniform keys)
./build/indexbench --lock ttas --dist uniform --threads 8 --read_pct 80 --insert_pct 10

# concurrent hash index (Zipfian skew)
./build/indexbench --lock occ --dist zipfian --threads 8 --read_pct 80 --insert_pct 10

# correctness tests (all locks)
./build/locktest

# correctness test (specific lock, custom params)
./build/locktest --lock ticket --threads 8 --loops 200000
```

## Sweep Scripts

```bash
# raw lock sweep across thread counts (results → results/lockbench.csv)
./scripts/sweep.sh [seconds] [max_threads] [repeats]

# index benchmark sweep (results → results/indexbench.csv)
./scripts/index_sweep.sh [seconds] [max_threads] [repeats]

# array benchmark sweep (results → results/arraybench.csv)
./scripts/array_sweep.sh [seconds] [max_threads] [repeats]

# example: 3s measurement, 6 threads max, 5 repeats
./scripts/sweep.sh 3 6 5
```

On Linux, sweep scripts auto-detect the platform and pass `--pin` to each run.
For best reproducibility on Linux, lock the CPU frequency first:

```bash
sudo ./scripts/setup_cpu.sh            # set performance governor, disable turbo
./scripts/sweep.sh 3 6 5
sudo ./scripts/setup_cpu.sh --reset    # restore defaults
```

## Assembly

Generate annotated ARM64 assembly for all lock primitives:

```bash
./scripts/gen_asm.sh           # outputs to asm/
./scripts/gen_asm.sh mydir     # custom output directory
```

Produces individual files per lock function (e.g. `asm/tas_lock.s`, `asm/occ_read_begin.s`).

## CLI Options

### lockbench

| Flag | Description | Default |
|------|-------------|---------|
| `--lock` | `tas\|ttas\|cas\|ticket\|rw\|occ\|rcu` | `tas` |
| `--workload` | `mutex\|rw\|rcu` | `mutex` |
| `--threads` | worker threads | hw_concurrency |
| `--seconds` | measurement duration | 3 |
| `--warmup` | warmup duration | 1 |
| `--cs_work` | busy-work loop iterations inside critical section | 0 |
| `--read_pct` | read % for rw/rcu workloads | 80 |
| `--pin` | pin threads to cores (Linux: `sched_setaffinity`, macOS: QoS hint) | off |
| `--csv` | append results as CSV to file | — |

### arraybench

| Flag | Description | Default |
|------|-------------|---------|
| `--lock` | `tas\|ttas\|cas\|ticket\|rw\|occ` | `ttas` |
| `--threads` | worker threads | hw_concurrency |
| `--seconds` | measurement duration | 3 |
| `--warmup` | warmup duration | 1 |
| `--num_locks` | number of locks in the array | 64 |
| `--cs_work` | busy-work loop iterations inside critical section | 0 |
| `--read_pct` | read % for rw/occ locks | 80 |
| `--pin` | pin threads to cores (Linux: `sched_setaffinity`, macOS: QoS hint) | off |
| `--csv` | append results as CSV to file | — |

### locktest

| Flag | Description | Default |
|------|-------------|---------|
| `--lock` | `tas\|ttas\|cas\|ticket\|rw\|occ` | all |
| `--threads` | worker threads | hw_concurrency |
| `--loops` | iterations per thread | 100000 |

### indexbench

| Flag | Description | Default |
|------|-------------|---------|
| `--lock` | `tas\|ttas\|cas\|ticket\|rw\|occ` | `ttas` |
| `--dist` | `uniform\|zipfian` | `uniform` |
| `--threads` | worker threads | hw_concurrency |
| `--seconds` | measurement duration | 5 |
| `--warmup` | warmup duration | 2 |
| `--read_pct` | lookup % | 80 |
| `--insert_pct` | insert % (rest = deletes) | 10 |
| `--zipf_theta` | Zipfian skew | 0.99 |
| `--buckets` | hash table buckets (power of 2) | 65536 |
| `--key_range` | key space size | 1000000 |
| `--prefill` | keys to pre-insert | 500000 |
| `--pin` | pin threads to cores (Linux: `sched_setaffinity`, macOS: QoS hint) | off |
| `--csv` | append results as CSV to file | — |

## Project Structure

```
include/
  primitives/       lock implementations
    tas_lock.hpp     test-and-set
    ttas_lock.hpp    test-and-test-and-set
    cas_lock.hpp     compare-and-swap
    ticket_lock.hpp  ticket lock
    rw_lock.hpp      reader-writer lock
    occ.hpp          optimistic concurrency (seqlock)
    rcu.hpp          epoch-based RCU
    util.hpp         cpu_relax(), start_barrier, busy_work()
  indexes/
    hash_index.hpp   concurrent hash table (per-bucket locking)
  util/
    zipfian.hpp      Zipfian distribution generator
bench/
  main.cpp           lockbench entry point
  array_bench.cpp    arraybench entry point
  index_bench.cpp    indexbench entry point
  lock_test.cpp      locktest entry point (correctness tests)
  lock_asm.cpp       source for assembly generation
scripts/
  sweep.sh           raw lock sweep
  index_sweep.sh     index benchmark sweep
  array_sweep.sh     array benchmark sweep
  gen_asm.sh         generate assembly files
  setup_cpu.sh       Linux CPU frequency locking (requires root)
results/
  arm_xgene1_lockbench.csv    X-Gene 1 (ARMv8) lockbench sweep results
  x86_xeon_lockbench.csv      Intel Xeon (x86_64) lockbench sweep results
  presentation_notebook.ipynb current ARM vs x86 comparison plots
  archive/                    earlier sweeps and notebooks kept for reference
EXPERIMENT.md                 detailed results and analysis
```

## Results

See [EXPERIMENT.md](EXPERIMENT.md) for detailed benchmark results, cross-architecture
comparisons (ARM64 vs x86_64), and assembly analysis.

To reproduce the analysis plots:

```bash
cd results
python3 -m venv .venv && source .venv/bin/activate
pip install ipykernel pandas matplotlib numpy
jupyter notebook presentation_notebook.ipynb
```
