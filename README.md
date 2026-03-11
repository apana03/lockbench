# lockbench

Benchmarks for synchronization primitives in the context of concurrent data structures.

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

- **lockbench** — raw lock/unlock throughput with configurable critical section work
- **arraybench** — contention on an array of cache-line-padded locks (exclusive, RW, or OCC)
- **indexbench** — concurrent hash table with per-bucket locking, uniform/Zipfian keys

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

# lock array (OCC with 80% read_lock / 20% write_lock per-operation probability)
./build/arraybench --lock occ --threads 4 --num_locks 64 --read_pct 80

# concurrent hash index (uniform keys)
./build/indexbench --lock ttas --dist uniform --threads 8 --read_pct 80 --insert_pct 10

# concurrent hash index (Zipfian skew)
./build/indexbench --lock occ --dist zipfian --threads 8 --read_pct 80 --insert_pct 10
```

## CSV Output

All benchmarks support `--csv <file>` to append results in CSV format. The header row is auto-created if the file is empty or missing. Sweep scripts write to `results/`.

```bash
./build/lockbench --lock ttas --workload mutex --threads 4 --csv results/lockbench.csv
```

## Sweep Scripts

```bash
# raw lock sweep across thread counts (results → results/lockbench.csv)
./scripts/sweep.sh [seconds] [max_threads]

# index benchmark sweep (results → results/indexbench.csv)
./scripts/index_sweep.sh [seconds] [max_threads]

# array benchmark sweep (results → results/arraybench.csv)
./scripts/array_sweep.sh [seconds] [max_threads]
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
| `--csv` | append results as CSV to file | — |

### arraybench

Each thread picks a random lock from the array, acquires it, optionally does busy work, and releases. Locks are cache-line padded (`alignas(64)`) to isolate true contention from false sharing. For RW and OCC locks, `--read_pct` controls the per-operation probability of using `read_lock()` vs `write_lock()` — every lock in the array sees the same statistical read/write mix.

| Flag | Description | Default |
|------|-------------|---------|
| `--lock` | `tas\|ttas\|cas\|ticket\|rw\|occ` | `ttas` |
| `--threads` | worker threads | hw_concurrency |
| `--seconds` | measurement duration | 3 |
| `--warmup` | warmup duration | 1 |
| `--num_locks` | number of locks in the array | 64 |
| `--cs_work` | busy-work loop iterations inside critical section | 0 |
| `--read_pct` | read % for rw/occ locks | 80 |
| `--csv` | append results as CSV to file | — |

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
| `--csv` | append results as CSV to file | — |

## Critical Section Simulation

Critical section work is simulated via `busy_work(iters)` — a tight loop with a compiler barrier (`asm volatile("" ::: "memory")`) that prevents optimization. Each iteration is roughly 1–2 cycles on ARM64. This avoids the overhead of timer-based approaches (`chrono::steady_clock`) which can dominate short critical sections.

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
  lock_asm.cpp       source for assembly generation
scripts/
  sweep.sh           raw lock sweep
  index_sweep.sh     index benchmark sweep
  array_sweep.sh     array benchmark sweep
  gen_asm.sh         generate assembly files
EXPERIMENT.md        detailed results and analysis
```

## Results

See [EXPERIMENT.md](EXPERIMENT.md) for detailed benchmark results and assembly analysis.
