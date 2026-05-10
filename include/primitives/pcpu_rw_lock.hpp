#pragma once
#include <atomic>
#include <cstdint>
#include "util.hpp"

// Per-CPU (per-thread-slot) reader-writer lock.
//
// Goal: a *truly* reader-scalable rwlock — readers do not bounce a shared
// cache line. Each thread is assigned a stable slot (TLS, lazy on first use),
// each slot holds its own reader counter on its own cache line, and the
// writer presence flag is a separate cache line.
//
// Reader fast path (no writer): one acquire-release RMW on the thread's own
// slot + one acquire load on writer_present (clean, no RMW). No cross-core
// cache traffic between readers.
//
// Writer slow path: O(N_SLOTS) — flip writer_present, then scan all slots
// and wait for them to drain to zero.
//
// Slots are heap-allocated so the object is small enough to fit the
// 128-byte wormhole shim storage by value.
//
// Reference: Linux's percpu_rwsem; Calciu et al. 2013 "NUMA-Aware Reader-
// Writer Locks" identifies the cache-line-bounce bottleneck in counter-based
// rwlocks that this design eliminates.
//
// Limitations (intentional, documented):
//   - Slots are assigned modulo N_SLOTS; once we exceed N_SLOTS distinct
//     threads, multiple threads share a slot and contend on that one line.
//     Set N_SLOTS to ≥ max benchmark thread count.
//   - Writers serialize via writer_present (no parallel writers).
//   - No TLS slot reclamation on thread exit; benchmark threads are stable
//     for the lifetime of the run.
struct pcpu_rw_lock {
  static constexpr int N_SLOTS = 64;

  struct alignas(64) reader_slot {
    std::atomic<std::int32_t> count{0};
    char pad[64 - sizeof(std::atomic<std::int32_t>)];
  };
  static_assert(sizeof(reader_slot) == 64, "reader_slot must be cache-line sized");

  std::atomic<bool> writer_present{false};
  reader_slot* slots;

  pcpu_rw_lock() : slots(new reader_slot[N_SLOTS]) {}
  ~pcpu_rw_lock() { delete[] slots; }
  pcpu_rw_lock(const pcpu_rw_lock&) = delete;
  pcpu_rw_lock& operator=(const pcpu_rw_lock&) = delete;

  // Process-wide TLS slot assignment. A thread's slot is stable for its lifetime
  // and shared across all pcpu_rw_lock instances in this process — a property of
  // the thread, not of the lock.
  static int my_slot() noexcept {
    thread_local int s = -1;
    if (s < 0) {
      static std::atomic<int> next{0};
      int idx = next.fetch_add(1, std::memory_order_relaxed);
      s = idx % N_SLOTS;
    }
    return s;
  }

  void read_lock() noexcept {
    int s = my_slot();
    for (;;) {
      // Publish our presence first, then check writer_present. If a writer is
      // already there, retract and retry. acq_rel is needed so the writer's
      // scan of slot counts observes our increment, and so we observe the
      // writer's writer_present=true store.
      slots[s].count.fetch_add(1, std::memory_order_acq_rel);
      if (!writer_present.load(std::memory_order_acquire)) return;
      slots[s].count.fetch_sub(1, std::memory_order_release);
      // Spin until the writer is gone.
      while (writer_present.load(std::memory_order_relaxed)) cpu_relax();
    }
  }

  void read_unlock() noexcept {
    slots[my_slot()].count.fetch_sub(1, std::memory_order_release);
  }

  void write_lock() noexcept {
    bool expected = false;
    while (!writer_present.compare_exchange_weak(
               expected, true,
               std::memory_order_acquire, std::memory_order_relaxed)) {
      expected = false;
      cpu_relax();
    }
    // Wait for all readers to drain. acquire on the load pairs with the
    // reader's release in read_unlock.
    for (int i = 0; i < N_SLOTS; ++i) {
      while (slots[i].count.load(std::memory_order_acquire) != 0) cpu_relax();
    }
  }

  void write_unlock() noexcept {
    writer_present.store(false, std::memory_order_release);
  }

  // For shim compatibility (some wormhole call sites use lock()/unlock() via
  // exclusive primitives) — map to the writer side.
  void lock()   noexcept { write_lock(); }
  void unlock() noexcept { write_unlock(); }

  // Non-blocking variants. try_write_lock requires no writer is present AND
  // all reader counts are zero. try_read_lock retries until cleared of the
  // writer, returning false if a writer is currently present.
  bool try_write_lock() noexcept {
    bool expected = false;
    if (!writer_present.compare_exchange_strong(
            expected, true,
            std::memory_order_acquire, std::memory_order_relaxed))
      return false;
    for (int i = 0; i < N_SLOTS; ++i) {
      if (slots[i].count.load(std::memory_order_acquire) != 0) {
        // Couldn't drain readers — give up the writer flag.
        writer_present.store(false, std::memory_order_release);
        return false;
      }
    }
    return true;
  }

  bool try_read_lock() noexcept {
    int s = my_slot();
    slots[s].count.fetch_add(1, std::memory_order_acq_rel);
    if (!writer_present.load(std::memory_order_acquire)) return true;
    slots[s].count.fetch_sub(1, std::memory_order_release);
    return false;
  }
};
