# Index-Level Lock Experiment

## 1. Motivation

The existing benchmarks (`lockbench`, `arraybench`, `indexbench`) isolate lock
primitives in increasingly realistic settings, but none of them exercise the
internal coordination that real indexes require:

- **`lockbench`** — one global lock, no data structure. Measures raw atomic cost.
- **`arraybench`** — array of `N` independent locks chosen at random. Measures
  striped contention with no structural coupling between operations.
- **`indexbench`** — per-bucket-locked hash table. One lock per operation, no
  traversal. Contention looks like `arraybench` with a deterministic mapping.

A concurrent index that isn't a hash table must **traverse** under contention.
Lookups walk multiple nodes; inserts have to extend and re-link structure;
deletes have to coordinate across ancestors. This exposes behavior that
per-bucket locking cannot:

- **Lock coupling / hand-over-hand** — holding two locks simultaneously to pass
  through a boundary.
- **Hot-path contention** on a shared root or leftmost path.
- **Shared-read benefit** for `rw_lock` and the **validation cost** for `occ`.
- **Lock-ordering discipline** required to stay deadlock-free.

This experiment adds two per-node-locked indexes — a **skip list** and a
**B+ tree** — and sweeps all six primitives (`tas`, `ttas`, `cas`, `ticket`,
`rw`, `occ`) through the same r/w workloads the hash table already uses.
`rcu` is deferred.

## 2. Code Structure

```
lockbench/
├── include/
│   ├── primitives/                  # lock implementations (unchanged)
│   │   ├── tas_lock.hpp             # test-and-set spinlock
│   │   ├── ttas_lock.hpp            # test-and-test-and-set spinlock
│   │   ├── cas_lock.hpp             # compare-and-swap spinlock
│   │   ├── ticket_lock.hpp          # FIFO ticket lock
│   │   ├── rw_lock.hpp              # reader-writer lock (shared/exclusive)
│   │   ├── occ.hpp                  # seqlock-style optimistic lock
│   │   └── util.hpp                 # cpu_relax, barriers, thread setup
│   ├── indexes/
│   │   ├── hash_index.hpp           # existing per-bucket-locked hash table
│   │   ├── skiplist_index.hpp       # NEW: per-node-locked skip list
│   │   └── bptree_index.hpp         # NEW: per-node-locked B+ tree (crabbing)
│   └── util/
│       ├── bench_harness.hpp        # NEW: shared harness for all index bins
│       └── zipfian.hpp              # Zipfian key generator
├── bench/
│   ├── index_bench.cpp              # hash table driver (uses bench_harness.hpp)
│   ├── skiplist_bench.cpp           # NEW: skiplist driver
│   ├── bptree_bench.cpp             # NEW: bptree driver
│   └── index_test.cpp               # NEW: correctness probe (oracle + race)
├── scripts/
│   ├── index_sweep.sh               # hash table sweep
│   ├── skiplist_sweep.sh            # NEW: skip list sweep
│   └── bptree_sweep.sh              # NEW: B+ tree sweep
└── CMakeLists.txt                   # adds skiplistbench, bptreebench, indextest
```

### 2.1 Lock-primitive interface (the contract)

Every lock exposes at minimum:

```cpp
void lock()      noexcept;   // exclusive acquire
void unlock()    noexcept;   // exclusive release
```

Two primitives additionally expose a shared path:

```cpp
// rw_lock
void read_lock()   noexcept;
void read_unlock() noexcept;

// occ_lock (seqlock-style)
Version read_begin()            const noexcept;   // load version
bool    read_validate(Version)  const noexcept;   // re-check version
// occ_lock::lock() is aliased to write_lock() — so write paths are uniform
```

The indexes use **SFINAE** to expose shared / optimistic read paths **only**
when the lock supports them:

```cpp
template <class L = Lock>
auto get_shared(key_type key) noexcept
    -> decltype(std::declval<L>().read_lock(), std::optional<value_type>{}) { ... }

template <class L = Lock>
auto get_optimistic(key_type key) noexcept
    -> decltype(std::declval<const L>().read_begin(), std::optional<value_type>{}) { ... }
```

This is the same pattern `hash_index<Lock>` uses, and it lets the benchmark
driver dispatch the right read function per lock type without `if constexpr`
leaking into the core data structure.

### 2.2 `skiplist_index<Lock>` — per-node hand-over-hand

A Herlihy–Shavit-style lazy skip list with a per-node `Lock`, `MAX_LEVEL = 16`,
and sentinel head/tail nodes flagged as `-∞ / +∞`.

```cpp
struct node {
  alignas(64) Lock  lock{};
  key_type          key;
  value_type        val;
  int               top_level;          // immutable after construction
  std::atomic<bool> marked{false};      // logically deleted
  std::atomic<bool> fully_linked{false};
  bool              is_neg_inf = false; // sentinels
  bool              is_pos_inf = false;
  node*             next[MAX_LEVEL]{};
};
```

Three read paths, one write path, one remove path:

| Method | Availability | Strategy |
|---|---|---|
| `get`             | all locks | multi-level hand-over-hand, exclusive |
| `get_shared`      | `rw_lock` only | same, using `read_lock`/`read_unlock` |
| `get_optimistic`  | `occ_lock` only | lock-free multi-level descent with per-node version validation |
| `put`             | all locks | unlocked `find`, lock unique preds **top-down** across levels, validate, link bottom-up, set `fully_linked` |
| `remove`          | all locks | unlocked `find`, lock unique preds top-down, lock victim, set `marked`, unlink top-down |

**Lock-ordering discipline.** The skip list avoids deadlock by enforcing a
single global order: **acquire locks left-to-right in list order**. Because
higher levels' predecessors are further left than lower levels', writers
iterate `lvl = top ... 0` (top-down = leftmost first). Readers walk the same
way — upper levels first, then level 0, hand-over-hand — so `get` and
`put`/`remove` never fight over the same pair of nodes in opposite orders.

**Reclamation.** Deleted nodes are **leaked**. This keeps the OCC read path
safe: readers can always dereference any pointer they observe because the
target is guaranteed never freed. Benchmarks are time-bounded (seconds), so
leakage is bounded.

### 2.3 `bptree_index<Lock, FANOUT = 16>` — top-down lock coupling (crabbing)

A simple B+ tree: leaves hold `(key, val)` pairs; internal nodes hold
`(keys[n], children[n+1])`. Single node type with an `is_leaf` discriminator.

```cpp
struct node {
  alignas(64) Lock  lock{};
  bool              is_leaf;
  std::uint16_t     n;
  key_type          keys[FANOUT];
  value_type        vals[FANOUT];        // leaf only
  node*             children[FANOUT+1];  // internal only
  node*             next_leaf = nullptr; // reserved for range scans
};
std::atomic<node*> root_;
Lock               root_latch_;          // held across root-replacement
```

Five entry points:

| Method | Descent | Lock footprint |
|---|---|---|
| `get`             | exclusive lock coupling | at most 2 node locks |
| `get_shared`      | shared lock coupling (rw) | at most 2 node read-locks |
| `get_optimistic`  | lock-free version-validated descent (occ) | 0 locks |
| `put`             | exclusive crabbing with safe-child release | O(log N) on unsafe paths, 2 on safe paths |
| `remove`          | exclusive lock coupling (no merge) | at most 2 node locks |

**Insert "safe child" rule.** At each level, if the chosen child is **not
full** (`child.n < FANOUT`), it cannot split on this insert, so every
ancestor's lock can be released before descending. This collapses the
common-case insert to exactly two locks held: current and child. If the child
is full, the writer keeps every ancestor latched; on a leaf split the new
separator is propagated up through the held stack. If the root itself splits,
a new root is allocated and installed via `root_.store(…, release)` while the
`root_latch_` is still held.

**Remove.** Simplified to plain exclusive lock coupling — no merging,
no rebalancing. Under-full nodes accumulate. This is acceptable for
time-bounded benchmarks and keeps the code path small enough to stay
auditable.

### 2.4 Shared benchmark harness (`include/util/bench_harness.hpp`)

Extracted from `bench/index_bench.cpp` and reused verbatim by all three index
binaries. Defines:

- `struct params` — all CLI parameters plus defaults.
- `parse_bench_args(argc, argv)` — CLI parser.
- `struct thread_stats { gets; puts; removes; }` (cache-line aligned).
- `run_bench_common<Index>(params, label, index, get_fn, put_fn, remove_fn)` —
  spawns `params.threads` workers, start-barrier-synchronized, each thread
  draws a key from uniform or Zipfian, picks an op by `read_pct / insert_pct`,
  and runs the caller-supplied lambda against the shared `index`.
- `prefill_index(index, params)` — uniform prefill before the barrier.
- `print_bench_result` / `csv_append` — stdout + CSV emission.

The three index drivers (`index_bench.cpp`, `skiplist_bench.cpp`,
`bptree_bench.cpp`) are nearly identical: each has three dispatch functions
(exclusive / rw / occ) and a `main` that wires the `--lock` flag to them. The
CSV schema is identical across binaries, so existing analysis keeps working;
only the output filename identifies the structure.

### 2.5 Correctness probe (`bench/index_test.cpp`)

Built as `indextest`. Two modes, both dispatched across
`{skiplist, bptree} × {tas, ttas, cas, ticket, rw, occ}`:

- **`--mode single`** — single-threaded oracle test against `std::map`.
  Random 40/35/25 `get`/`put`/`remove` mix for N ops, each `get` is
  cross-checked with the oracle, final sweep re-checks every live oracle key.
  For the B+ tree, also runs **split probes** across sizes
  `{1, 16, 17, 256, 257, 1000, 10000}` to straddle fanout boundaries and
  force internal / root splits.
- **`--mode race`** — N threads on **disjoint** key slices
  (`[t·K, (t+1)·K)`). Each thread tracks its own "present" set; after join,
  a single-threaded sweep asserts every key the thread believes it put
  (and didn't later remove) is still present. No lost updates, no crashes,
  no hangs.

Race mode is the important one: by partitioning the key space, any structural
bug (lost link, orphaned node, deadlock, unmarked delete) surfaces as a
missing key or a hang — not as a subtle value mismatch that's easy to miss.

## 3. Experiment Setup

### 3.1 Parameter surface

Each of the three benchmark binaries (`indexbench`, `skiplistbench`,
`bptreebench`) accepts:

| Flag | Default | Meaning |
|---|---|---|
| `--lock`       | `ttas`    | `tas`\|`ttas`\|`cas`\|`ticket`\|`rw`\|`occ` |
| `--dist`       | `uniform` | `uniform`\|`zipfian` |
| `--threads`    | hw conc.  | worker threads |
| `--seconds`    | 5         | measurement window |
| `--warmup`     | 2         | warmup before measurement starts |
| `--read_pct`   | 80        | `get` fraction (0..100) |
| `--insert_pct` | 10        | `put` fraction; remove = 100 − read − insert |
| `--zipf_theta` | 0.99      | Zipf skew (0 = uniform, 0.99 = very skewed) |
| `--key_range`  | 1,000,000 | key universe |
| `--prefill`    | 500,000   | keys inserted uniformly before measurement |
| `--buckets`    | 65,536    | hash-only; ignored for skiplist / bptree |
| `--pin`        | off       | `sched_setaffinity` on Linux |
| `--csv`        | —         | append CSV row |

Fanout (`FANOUT = 16`) and skip list height (`MAX_LEVEL = 16`) are
compile-time. Sweeping them is a deliberate follow-up.

### 3.2 Workload matrix (sweep scripts)

`scripts/skiplist_sweep.sh` and `scripts/bptree_sweep.sh` both iterate:

```
locks    = {tas, ttas, cas, ticket, rw, occ}
threads  = {1, 2, 4, 8, 16, 32, ..., up to hw_concurrency}
repeats  = 3
sections:
  1. Uniform   80/10/10   (baseline)
  2. Zipfian   80/10/10   (hot-key contention, theta=0.99)
  3. Uniform   95/4/1     (read-heavy — exposes rw / occ advantage)
  4. Zipfian   20/50/30   (write-heavy — exposes writer contention)
```

Each run is `--seconds 3 --warmup 3`. On Linux the scripts pass `--pin`
automatically. Output goes to `results/skiplistbench.csv` and
`results/bptreebench.csv` — same schema as the existing hash-table CSV,
so the existing analysis (`results/xgene1_vs_xeon_analysis.py`, the Jupyter
notebook, etc.) consumes it without change.

### 3.3 What each workload isolates

| Section | What it stresses |
|---|---|
| Uniform 80/10/10   | Baseline — balanced mix, no key-space skew. Measures steady-state throughput. |
| Zipfian 80/10/10   | **Hot-key contention.** A few keys dominate, so the same nodes are hit repeatedly — stresses per-node lock arbitration and (for bptree) near-root coupling. |
| Uniform 95/4/1     | **Reader parallelism.** Isolates `rw.get_shared` and `occ.get_optimistic`; writers stay rare enough that shared/optimistic paths aren't invalidated. |
| Zipfian 20/50/30   | **Writer contention.** High write rate on skewed keys — exposes OCC validation-retry storms and writer starvation on FIFO vs non-FIFO locks. |

### 3.4 Correctness gate

Before any benchmark run, the full correctness matrix must pass:

```bash
for s in skiplist bptree; do
  for l in tas ttas cas ticket rw occ; do
    ./build/indextest --structure $s --lock $l --mode single
    ./build/indextest --structure $s --lock $l --mode race --threads 8
  done
done
```

All 24 combinations must report `OK`. This is the smoke test that catches
deadlocks, lost-update bugs, and structural corruption before a sweep burns
minutes of CPU time.

### 3.5 Reproducing

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Correctness
for s in skiplist bptree; do
  for l in tas ttas cas ticket rw occ; do
    ./build/indextest --structure $s --lock $l --mode single
    ./build/indextest --structure $s --lock $l --mode race --threads 8
  done
done

# Smoke bench
./build/skiplistbench --lock ttas --threads 4 --seconds 2 --warmup 1
./build/bptreebench   --lock rw   --threads 4 --seconds 2 --warmup 1

# Full sweeps (3s per run, hw_concurrency threads, 3 repeats)
./scripts/skiplist_sweep.sh 3 "" 3
./scripts/bptree_sweep.sh   3 "" 3
```

On Linux, lock CPU frequency first with `sudo ./scripts/setup_cpu.sh` to cut
variance; the sweep scripts pass `--pin` automatically.

## 4. Deliberate Simplifications

| Area | Choice | Why |
|---|---|---|
| Memory reclamation | Leak deleted nodes | OCC readers must be able to dereference any pointer they observe; benchmark runs are time-bounded. |
| B+ tree remove | No merges / no rebalance | Keeps the remove path auditable. Under-full nodes are tolerable for bounded runs. |
| Fanout / max level | Fixed at 16 at compile time | Avoids a template-instantiation matrix; sweeping fanout is a separate experiment. |
| RCU | Excluded | Not part of this pass — epoch reclamation would let us drop the "leak" simplification, but it's a separate change. |
| Skip list read path | Multi-level hand-over-hand (not just level 0) | Level-0-only was O(N) and ~20× slower than bptree; multi-level restores O(log N). |

## 5. Files Produced by This Experiment

- `include/indexes/skiplist_index.hpp`
- `include/indexes/bptree_index.hpp`
- `include/util/bench_harness.hpp`
- `bench/skiplist_bench.cpp`
- `bench/bptree_bench.cpp`
- `bench/index_test.cpp`
- `scripts/skiplist_sweep.sh`
- `scripts/bptree_sweep.sh`
- CMake targets: `skiplistbench`, `bptreebench`, `indextest`
- Output: `results/skiplistbench.csv`, `results/bptreebench.csv` (same schema
  as `results/indexbench.csv`)
