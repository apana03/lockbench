# How Wormhole Was Adapted for Lockbench

This document records what was changed in (and around) Wu et al.'s
[Wormhole](https://github.com/wuxb45/wormhole) (FAST '19) to fold it into
the lockbench benchmark suite, where the goal is to measure how lock
primitive choice affects index throughput across StripedMap, BronsonAVL,
and Wormhole.

The adaptation has two layers:

1. **Lock-primitive swap** — replace wormhole's `rwlock` and `spinlock`
   bodies with any of the lockbench primitives, selected at compile
   time. This is the bulk of the work and the headline goal.
2. **Optimistic OCC reader** — an additional variant (`wh-occ-opt`)
   that adds a per-leaf seqlock counter and a lock-free reader path,
   so we can compare an OCC-style optimistic read against rwlock-based
   shared reads.

All upstream wormhole code is preserved on the `default` build path —
disabling `WH_LOCK_SHIM` produces a binary that runs Wu's stock locks
unmodified. Every adaptation is gated on a compile-time macro.

---

## 1. Vendoring

Wormhole is cloned in-tree under `third_party/wormhole/` as an editable
copy (no submodule — we apply small surgical patches and don't want
detached state). Upstream files preserved; the only edits to upstream
sources are `#ifdef`/`#ifndef` guards or short blocks gated by our
macros.

**Files in `third_party/wormhole/`:**

| File | Origin | Modified? |
|------|--------|-----------|
| `lib.h`, `lib.c`, `wh.h`, `wh.c`, `kv.h`, `kv.c` | upstream | yes — guarded patches only |
| `Makefile`, `Makefile.common` | upstream | unchanged (we use CMake) |
| `wh_lock_shim.h` | new | added by us |
| `wh_lock_shim.cpp` | new | added by us |

Total upstream diff: ~30 lines, all `#ifdef`-guarded. No upstream
function bodies were rewritten; we only inject new code paths.

## 2. Build wiring (CMake)

`CMakeLists.txt` builds one `wormhole-rt-<lk>` static library + matching
`wh_bench_<lk>` and `wh_test_<lk>` executables per variant. The variant
list is:

```cmake
set(WH_LOCKS default rw tas ttas cas occ occ-opt)
```

- `default` — upstream wormhole, **no shim**.
- `rw` / `tas` / `ttas` / `cas` / `occ` — shim active, lock body
  selected via `-DWH_LOCK_<NAME>` macro.
- `occ-opt` — shim active with `-DWH_LOCK_CAS` for the writer side,
  plus `-DWH_OCC_OPTIMISTIC` to enable the optimistic reader path.
- `ticket` is **deliberately excluded** — see §6.

Per-variant defines:

```cmake
if (lk STREQUAL "default")
  add_library(${libname} STATIC ${WH_C_SRCS})  # no shim
elseif (lk STREQUAL "occ-opt")
  add_library(${libname} STATIC ${WH_C_SRCS} ${WH_LOCK_SHIM_CPP})
  target_compile_definitions(${libname} PUBLIC
      WH_LOCK_SHIM WH_LOCK_CAS WH_OCC_OPTIMISTIC)
else()
  add_library(${libname} STATIC ${WH_C_SRCS} ${WH_LOCK_SHIM_CPP})
  target_compile_definitions(${libname} PUBLIC WH_LOCK_SHIM WH_LOCK_${LK_UPPER})
endif()
target_compile_definitions(${libname} PUBLIC NOSIGNAL)
```

`NOSIGNAL` is required (see §7).

## 3. The lock shim

Wormhole declares `struct rwlock` and `struct spinlock` as small
opaque-`u32` unions in `lib.h:304-322`, with sixteen-ish C-callable
functions (`rwlock_lock_read`, `rwlock_trylock_write_nr`, etc.). Our
shim provides drop-in replacements **with the exact same names and
signatures** so wormhole's call sites compile unchanged.

### `wh_lock_shim.h`

Declares `struct rwlock { _Alignas(64) unsigned char storage[128]; }`
and `struct spinlock { _Alignas(64) unsigned char storage[128]; }`
plus forward declarations for the rwlock_*/spinlock_* functions
matching upstream. 128 bytes is enough to hold our largest primitive
(`ticket_lock` with two `alignas(64)` atomic counters).

### `wh_lock_shim.cpp`

C++ TU that:

1. Picks the lock type via `WH_LOCK_<NAME>` macro:
   ```cpp
   #if   defined(WH_LOCK_RW)     using LockT = rw_lock;     constexpr bool kIsRw = true;
   #elif defined(WH_LOCK_TAS)    using LockT = tas_lock;    constexpr bool kIsRw = false;
   ...
   ```
2. Asserts `sizeof(LockT) ≤ sizeof(rwlock::storage)`.
3. Provides `extern "C"` definitions for every rwlock_*/spinlock_*
   function. They placement-new `LockT` into the opaque storage at
   `_init` time and dispatch via template helpers (`do_read_lock<L>`,
   `try_excl<L>`, etc.) so `if constexpr` actually elides non-matching
   branches at instantiation time.
4. Handles the BasicLockable adapters: for `tas`/`ttas`/`cas`/`occ`
   (no native rwlock), `lock_read` aliases to `lock` and reader
   concurrency is lost. This is intentional — measuring "what happens
   when readers serialize" is part of the experiment.

### Upstream guards

In `lib.h` we wrap the upstream `struct rwlock`/`struct spinlock` and
their function declarations in:

```c
#ifdef WH_LOCK_SHIM
#  include "wh_lock_shim.h"
#else
   /* original definitions */
#endif
```

In `lib.c` we wrap the spinlock implementation block (~lines 1108-1182)
and the rwlock implementation block (~lines 1316-1525) in
`#ifndef WH_LOCK_SHIM` so the C-side bodies are skipped under the
shim. Upstream's `pthread_mutex_*` and `rwdep` (rwlock-dependency
tracking) sections are unchanged.

In `wh.c` two literal-initializer accesses to `leaf->leaflock.opaque`
(lines ~462 and ~2203) are wrapped in `#ifndef WH_LOCK_SHIM` blocks; on
the shim path we use `rwlock_init(&leaf->leaflock)` instead.

## 4. The optimistic OCC reader (`wh-occ-opt`)

This variant adds a real lock-free reader path while keeping writers
serialized through an exclusive lock (`cas_lock` underneath). All
changes are gated by `#ifdef WH_OCC_OPTIMISTIC` and are no-ops under
any other variant.

### Patch points (all in `wh.c`)

1. **`struct wormleaf` field (line ~52)**

   Add a per-leaf 64-bit atomic seqlock counter, sharing space with
   the existing `reserved[2]` slot so the layout stays the same:

   ```c
   #ifdef WH_OCC_OPTIMISTIC
     au64 occ_seq;
     u64 reserved[1];
   #else
     u64 reserved[2];
   #endif
   ```

2. **`wormleaf_alloc` (line ~459)** — initialize `occ_seq` to 0.
   `slab_alloc_safe` returns memory with stale bits; without explicit
   init, fresh leaves can start with an odd seq and readers spin
   forever waiting for a "writer" that never existed.

3. **`wormleaf_lock_write` / `wormleaf_unlock_write` (lines ~547, ~570)** —
   bump `occ_seq` to odd at lock acquire, back to even at release:

   ```c
   atomic_fetch_add_explicit(&leaf->occ_seq, 1, MO_RELEASE);
   ```

4. **`wormhole_jump_leaf_write` (line ~1429)** — wormhole's writer fast
   path bypasses `wormleaf_lock_write` and calls `rwlock_trylock_write_nr`
   directly. Without a matching `occ_seq` bump here, every
   put/erase/update would leave the leaf in an "odd seq" state once
   the matching `wormleaf_unlock_write` runs. We bump unconditionally
   on `trylock_write_nr` success so the unlock balances correctly.
   This is the most subtle of the patches.

5. **`wormhole_split_insert` (line ~2548)** — bypasses `wormleaf_lock_write`
   in the same way for the freshly-allocated `leaf2`. Same fix.

6. **`wormhole_get` (line ~2237)** — replaced under `#ifdef WH_OCC_OPTIMISTIC`
   with a seqlock-validated lock-free reader using a custom hs[] scan
   (see "Avoiding torn entry13 reads" below):

   ```c
   do {
       hmap = wormhmap_load(map);
       v = wormhmap_version_load(hmap);
       qsbr_update(&ref->qref, v);
       leaf = wormhole_jump_leaf(hmap, key);
       if (wormleaf_version_load(leaf) > v) continue;
       seq_start = atomic_load_explicit(&leaf->occ_seq, MO_ACQUIRE);
       if (seq_start & 1) continue;             // writer in progress
       i = wormleaf_match_hs(leaf, key);
       tmp = (i < WH_KPN) ? map->mm.out(wormleaf_kv_at_ih(leaf, i), out) : NULL;
       seq_end = atomic_load_explicit(&leaf->occ_seq, MO_ACQUIRE);
       if (seq_start == seq_end && wormleaf_version_load(leaf) <= v)
           return tmp;                          // success
       // mismatch → retry
   } while (true);
   ```

   No `rwlock_lock_read` is taken. Memory safety relies on QSBR
   (the leaf can't be freed while we hold a `wormref`), and on
   the seqlock validate to retry on torn reads of `nr_keys`/`ss[]`.

### Avoiding torn entry13 reads

`struct entry13` is the 8-byte packed (e1: u16 key prefix, e3: u48
compressed pointer) used to store keys in a leaf's `hs[]` array.
Wormhole's stock `wormleaf_match_hs` reads `hs[i].e1` and `hs[i].e3`
as **separate** field accesses. With the optimistic reader (no
leaflock), this opens a UAF window:

1. Reader reads `hs[i].e1 == pkey` (matches the search key).
2. Concurrent writer's `wormleaf_remove` does `hs[i].v64 = 0`
   (atomic 8-byte clear).
3. Reader reads `hs[i].e3 = 0`, dereferences NULL → segfault.

The seqlock validate would catch the inconsistency on the *next*
iteration — but only after the segfault.

Fix: read each `entry13` as a single atomic 8-byte load via `v64`,
then unpack `e1` and `e3` from the same snapshot:

```c
const u64 ev = atomic_load_explicit((const _Atomic u64 *)&hs[idx].v64,
                                    MO_ACQUIRE);
const u16 e1 = (u16)(ev & 0xFFFFu);
const u64 e3 = ev >> 16;
```

The implementation mirrors wormhole's stock `wormleaf_match_hs` —
hash-positional probe at `i0 = pkey / WH_HDIV`, then walk left
(while e1 ≥ pkey) and right (while e1 ≤ pkey, e1 ≠ 0). Each entry
read uses an atomic v64 load. **Same O(1) expected lookup as upstream**;
the only differences are atomic loads (vs plain field reads) and
that we deref the kv only after confirming `curr != NULL` (defensive
since stale snapshots may give us a freed-but-still-mapped kv from
an earlier writer).

If a concurrent writer's mid-shift creates entries that violate the
hash-positional ordering, our walk may break early and "miss" the
key. The seqlock validate at the end of the outer retry loop catches
this — `seq_start != seq_end` triggers a replay.

(An earlier draft used a *linear scan* over all WH_KPN=128 entries.
That was a defensive overcorrection — correct but ~10× slower per
read than the hash-indexed walk, and dominated the read-heavy
benchmark results until we realized the hash-indexed approach was
also safe. See `EXPERIMENT.md` for the before/after numbers.)

### kv reclamation under optimistic reads

Wormhole's stock `kvmap_mm_dup` frees old kvs immediately on update or
delete. With locked readers (the original wormhole_get path), this is
safe — the writer holds `leaflock_write` and no reader can have a
pointer to the kv being freed.

Under optimistic reads, this protection is gone. A reader may snapshot
a kv pointer P, get pre-empted, and then the next concurrent writer
free P before the reader dereferences it (in `mm.out` / `kv_dup2`).
The seqlock validate would catch the concurrent write — but only
*after* the dereference. UAF crash.

For the `wh-occ-opt` variant, the C++ adapter
(`include/indexes/wormhole_index.hpp`) installs a custom `kvmap_mm`:

```cpp
inline void occopt_mm_free_noop(struct kv* const, void*) {
    // intentional no-op: defer free indefinitely
}
inline const struct kvmap_mm occopt_mm = {
    occopt_mm_in_dup, occopt_mm_out_dup, occopt_mm_free_noop, nullptr,
};
```

`free` is a no-op, so kvs leak for the lifetime of the process.
For a 3-second bench at ~50 M ops/s with ~10% inserts and 8-byte
key/value pairs, that's roughly 150 MB of leaked kvs — bounded and
acceptable. A production-quality optimistic reader would defer free
via QSBR (add a per-thread retire list, free after grace period).
That's a deeper change to wormhole's reclamation engine and out of
scope here.

### MM fairness across variants (`WH_FAIR_MM`)

The custom no-op-free MM is **required** for `wh-occ-opt`'s safety,
but it has a side effect: it eliminates the `free()` call that the
locked variants pay on every update and delete. On write-heavy
workloads with stock `kvmap_mm_dup`, locked variants pay ~10–30 ns
per op for `free()` that `wh-occ-opt` doesn't. This artificially
inflates `wh-occ-opt`'s apparent advantage by ~2× on zipfian
write-heavy mixes.

To control for this, CMake exposes a `WH_FAIR_MM` option:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DWH_FAIR_MM=ON
```

When set, **all** variants (not just `wh-occ-opt`) use the no-op-free
MM. This is the right configuration for comparing reader strategies
because it neutralizes allocator-cost asymmetries.

`-DWH_FAIR_MM=OFF` (the CMake default) leaves locked variants on
stock `kvmap_mm_dup`. That's appropriate when comparing locked
variants against each other (and against Wu's upstream baseline).
But for `wh-occ-opt` comparisons, always build with `WH_FAIR_MM=ON`
or you'll measure allocator behavior, not synchronization.

The `EXPERIMENT.md` results headline used `WH_FAIR_MM=ON`. The
unfair-MM numbers we briefly captured before fixing this showed
`wh-occ-opt` 4× faster than `wh-cas` on zipfian 20/40/40 — a
measurement artifact, not a real win.

### What the optimistic variant measures

- ✅ **Reader-reader scaling** — multiple readers can be inside the
  same leaf simultaneously. No exclusive-state cache-line bouncing
  for reads (unlike `wh-occ`, which CASes the OCC version).
- ✅ **Reader-writer overlap** — readers don't block on a writer; they
  just retry if validation fails.
- ❌ **Writer-side OCC** — writers still serialize through an exclusive
  CAS lock. This isn't a fully optimistic OCC implementation; it's
  lock-free reads on top of locked writes.
- ❌ **Iterators / range scans** — `wormhole_iter_*` wasn't touched.
  The optimistic patch is scoped to the point-lookup hot path.
- ❌ **Memory accounting** — the no-op free intentionally leaks. Don't
  measure RSS or running this for hours.

## 5. The C++ adapter

`include/indexes/wormhole_index.hpp` exposes the same `get/put/remove`
interface used by `hash_index` / `striped_map_index` / `avl_tree_index`
so the existing benchmark harness slots in unchanged.

Three subtleties:

### Per-thread `wormref` lifecycle

Wormhole requires every thread that touches the map to hold a `wormref`
(its QSBR registration handle). We use a `thread_local` guard:

```cpp
struct wh_thread_guard {
    struct wormhole* attached_map = nullptr;
    struct wormref*  ref          = nullptr;
    void ensure_current() {
        struct wormhole* cur = global_map_slot();
        if (cur == attached_map) return;
        if (ref && attached_map) wormhole_unref(ref);
        attached_map = cur;
        ref = cur ? wormhole_ref(cur) : nullptr;
    }
    ~wh_thread_guard() {
        if (ref && global_map_slot() == attached_map && attached_map)
            wormhole_unref(ref);
    }
};
```

Two specific bugs fixed here:

- **Stale ref across `wormhole_index` instances.** `--mode both` in
  `wh_test` constructs two `wormhole_index` instances sequentially.
  The thread-local guard's first ref was tied to instance #1; on
  instance #2 it would dereference a destroyed map. Fix:
  `ensure_current()` checks the global map pointer and re-attaches
  if the map has changed.
- **Cleanup-time deref.** The C++ runtime destroys `wormhole_index`
  (which calls `wormhole_destroy`) before TLS destructors run. The
  TLS guard's destructor would then call `wormhole_unref` on a
  destroyed map → wormhole's `debug_wait_gdb` handler triggers and
  the program sleeps forever waiting for a debugger. Fix: skip the
  unref if `global_map_slot() == nullptr`.

### Park / resume around every operation

Wormhole's QSBR uses each registered thread's quiescence state to
decide when freed leaves can be reclaimed. A registered-but-idle
thread (e.g., the main thread joining workers) blocks reclamation,
making `qsbr_wait` inside writer paths spin forever.

The adapter wraps each `get/put/remove` in
`wormhole_resume(r) ... wormhole_park(r)`. Threads are parked between
operations; QSBR doesn't wait for them. The 2 atomic ops per call
are negligible overhead.

### Big-endian key encoding

Keys are encoded as 8-byte big-endian byte strings so wormhole's
lexicographic order matches numeric order. Doesn't affect point
lookups but makes ordered/range queries sane if you ever add them.

## 6. Why ticket lock is excluded

A real ticket-lock acquire is destructive: `fetch_add` mortgages a
queue slot. There's no way to "give back" a ticket without breaking
the queue. So `try_lock` must succeed only when the queue is empty:

```cpp
bool try_lock() noexcept {
    auto cur_owner = owner.load(std::memory_order_relaxed);
    auto cur_next  = next.load(std::memory_order_relaxed);
    if (cur_next != cur_owner) return false;          // queue not empty
    return next.compare_exchange_strong(cur_next, cur_owner + 1,
        std::memory_order_acquire, std::memory_order_relaxed);
}
```

Wormhole's reader path uses `rwlock_trylock_write_nr(leaflock, 64)`.
Under any sustained writer activity, the queue is rarely empty, so
this `try_lock` effectively always fails — readers always fall to the
slow optimistic path. The result is dominated by the shim's deviation,
not the lock primitive itself, so we exclude `wh-ticket` from the
build list.

This is consistent with the same finding observed on BronsonAVLTreeMap,
where `avl-ticket` collapses to ≈0.9 M ops/s on write-heavy zipfian
because Bronson's reader-version protocol is similarly try-lock-based.

## 7. Gotchas during integration

### `NOSIGNAL` define

Wormhole installs `SIGSEGV/SIGFPE/SIGILL/SIGBUS` handlers (`lib.c`
`debug_init` constructor, gated on `!defined(NOSIGNAL)`) that, instead
of aborting, print "[SIGNAL] ..." to stderr and `nanosleep` forever
waiting for a debugger to attach. This makes any actual fault look
like a deadlock.

We `target_compile_definitions(... PUBLIC NOSIGNAL)` so faults
propagate normally. This is mandatory; without it, a real bug in
the adapter manifests as 0% CPU sleeping processes.

### `NDEBUG` / Release build

`debug_assert` in `lib.h` is gated on `!defined(NDEBUG)`. Without
NDEBUG, asserts that fail call into the same `wait_gdb_handler`
infinite-sleep path. CMake's `-DCMAKE_BUILD_TYPE=Release` defines
`NDEBUG` automatically, but a default (no build type) build does
**not**. Always build Release.

### Apple Silicon assembler quirk

`lib.c` has file-scope inline assembly defining `_co_switch_stack`
and `_co_entry_aarch64` (coroutine entry symbols, used by
`corr_yield`). The upstream version uses `;` as a statement separator:

```c
asm (
    ".global _co_switch_stack;"
    "_co_switch_stack:"
    "sub  x8, sp, 160;"
    ...
);
```

clang's macOS aarch64 assembler doesn't define `_co_switch_stack` as
a global symbol with this layout — the `;` is treated as a comment
character or otherwise doesn't end the directive properly. The fix is
to replace `;` with `\n` between `.global` and the label:

```c
".global _co_switch_stack\n"
"_co_switch_stack:\n"
"sub x8, sp, 160;"
```

Three sites needed this fix in `lib.c`. Linux/x86 builds were not
affected.

### Slab allocator doesn't zero memory

`slab_alloc_safe` returns memory with stale bits from prior allocations.
For the OCC-optimistic variant we explicitly initialize `occ_seq` to 0
in `wormleaf_alloc`; without this, fresh leaves can start with an odd
seq and the optimistic reader spins forever.

## 8. Verification

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ls build/wh_test_*           # 7 binaries

# Correctness
for lk in default rw tas ttas cas occ occ-opt; do
    ./build/wh_test_${lk} --mode both
done

# Stress (load-bearing for QSBR + occ-opt torn-read tolerance)
for lk in default rw tas ttas cas occ occ-opt; do
    ./build/wh_test_${lk} --mode race --threads 16 \
        --per_thread_keys 1024 --ops_per_thread 200000
done

# Sweep
./scripts/wh_compare.sh 3 1
wc -l results/wh_compare/wh.csv   # 1 header + 112 data rows

# Plot
.venv-plot/bin/jupyter nbconvert --to notebook --execute --inplace \
    scripts/avl_compare.ipynb
```

Pass criteria: all 7 binaries print `PASSED (0 check failures)` for
both correctness modes; the sweep CSV has 112 data rows; the notebook
re-executes without exceptions.

## 9. File map

| Path | Purpose |
|------|---------|
| `third_party/wormhole/lib.h`, `lib.c`, `wh.c` | Upstream wormhole, with guarded patches |
| `third_party/wormhole/wh_lock_shim.h`, `.cpp` | Drop-in lock body shim |
| `include/indexes/wormhole_index.hpp` | C++ adapter (`get`/`put`/`remove`) |
| `include/primitives/rw_lock.hpp` | Added `try_write_lock` + `try_read_lock` (pure additions) |
| `include/primitives/ticket_lock.hpp` | Added `try_lock` (empty-queue CAS; not used here) |
| `bench/wh_bench.cpp`, `wh_test.cpp` | Bench driver + correctness probe |
| `scripts/wh_compare.sh` | Per-variant sweep |
| `scripts/avl_compare.ipynb` | Plot the results alongside StripedMap and BronsonAVL |
| `EXPERIMENT.md` | Results + interpretation |
| `WORMHOLE_ADAPTATION.md` | This document |
