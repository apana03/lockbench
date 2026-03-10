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

- **lockbench** — raw lock/unlock throughput with configurable critical section
- **arraybench** — shared array with single-lock or striped-lock modes
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

# reader-writer workload
./build/lockbench --lock occ --workload rw --threads 4 --read_pct 95

# RCU workload
./build/lockbench --lock rcu --workload rcu --threads 4 --read_pct 95

# shared array (single global lock)
./build/arraybench --lock ttas --mode single --threads 4 --read_pct 80

# shared array (striped locks)
./build/arraybench --lock ttas --mode striped --threads 8 --stripes 64

# concurrent hash index (uniform keys)
./build/indexbench --lock ttas --dist uniform --threads 8 --read_pct 80 --insert_pct 10

# concurrent hash index (Zipfian skew)
./build/indexbench --lock occ --dist zipfian --threads 8 --read_pct 80 --insert_pct 10
```

## Sweep Scripts

```bash
# raw lock sweep across thread counts
./scripts/sweep.sh [seconds] [max_threads]

# index benchmark sweep (thread counts, distributions, read ratios)
./scripts/index_sweep.sh [seconds] [max_threads]

# array benchmark sweep (single/striped, read ratios)
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
| `--cs_ns` | busy-wait ns inside critical section | 0 |
| `--read_pct` | read % for rw/rcu workloads | 80 |

### arraybench

| Flag | Description | Default |
|------|-------------|---------|
| `--lock` | `tas\|ttas\|cas\|ticket\|rw\|occ` | `ttas` |
| `--mode` | `single\|striped` | `single` |
| `--threads` | worker threads | hw_concurrency |
| `--seconds` | measurement duration | 3 |
| `--warmup` | warmup duration | 1 |
| `--read_pct` | read % | 80 |
| `--array_size` | array elements | 65536 |
| `--stripes` | stripe count (striped mode) | 64 |
| `--scan_len` | elements per read scan | 16 |

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
    util.hpp         cpu_relax(), start_barrier, busy_wait_ns()
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
