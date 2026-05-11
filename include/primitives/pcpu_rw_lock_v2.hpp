#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include "util.hpp"

// Per-CPU reader-writer lock, percpu_rwsem-style (v2).
//
// This is a redesign of pcpu_rw_lock (v1) intended to fix the thundering-herd
// collapse documented in docs/INVESTIGATION_PCPU_RW.md. The reader-side data
// layout (cache-line-isolated counters per thread slot) is unchanged. The
// writer–reader handshake is what's different.
//
// Why v1 collapsed
// ----------------
// v1's slow path was a busy-wait on the writer_present flag. When a writer
// arrived, ALL active readers retracted in parallel and spun on writer_present.
// When the writer released, all readers raced back into the fast path
// simultaneously, often arriving before the next writer was even queued. With
// non-trivial writer rates (≥ 1 %), this oscillation dominated wall-clock and
// throughput collapsed by 1–3 orders of magnitude.
//
// What v2 changes
// ---------------
// The slow path queues on an OS-level mutex (futex-backed on Linux). Readers
// that observe a pending writer do NOT spin; they wait on writer_mu, which
// the writer holds for the duration of its critical section. Wake-up is then
// serialised by the mutex's queue policy, breaking the herd. Readers in the
// fast path are unaffected: a single per-CPU acq_rel RMW and an acquire load
// on writer_pending, same as v1.
//
// Reader fast path:
//   fetch_add(slots[t].count, 1, acq_rel);            // publish presence
//   if (writer_pending.load(acquire) == false) return; // common case
//   // slow path
//
// Reader slow path:
//   fetch_sub(slots[t].count, 1, release);            // retract
//   writer_mu.lock();                                 // wait on the queue
//   fetch_add(slots[t].count, 1, acq_rel);            // re-publish
//   writer_mu.unlock();
//
// Writer:
//   writer_mu.lock();                                 // serialise vs other writers
//   writer_pending.store(true, release);              // close fast path for new readers
//   for (i in 0..N_SLOTS-1) while slots[i].count != 0: cpu_relax();
//   ... CS ...
//   writer_pending.store(false, release);             // reopen fast path
//   writer_mu.unlock();
//
// Notes on correctness:
//   - Writers are serialised by writer_mu. While writer holds it, slow-path
//     readers are queued on the same mutex; new fast-path readers will observe
//     writer_pending=true and fall through to the slow path. So no reader
//     enters its critical section while the writer is in its CS.
//   - In-flight fast-path readers (those who did fetch_add before writer_pending
//     was set) are observed by the writer's drain scan. The acq_rel on the
//     reader's fetch_add and the release on writer_pending.store give the
//     necessary ordering for the writer's acquire load on the slot to see the
//     reader's increment.
//   - Writers can be temporarily starved by a steady stream of fast-path
//     readers, but this is bounded by the rate at which readers arrive and the
//     time their critical sections take. For our short CSes and < 50 % writer
//     workloads, this is negligible.
//
// Memory cost: ~56 bytes per lock instance for the on-stack/embedded part
// (writer_pending + std::mutex + slot pointer), plus a heap allocation of
// N_SLOTS × 64 B = 4 KiB for the slot array. Same as v1.
struct pcpu_rw_lock_v2 {
  static constexpr int N_SLOTS = 64;

  struct alignas(64) reader_slot {
    std::atomic<std::int32_t> count{0};
    char pad[64 - sizeof(std::atomic<std::int32_t>)];
  };
  static_assert(sizeof(reader_slot) == 64, "reader_slot must be cache-line sized");

  std::atomic<bool> writer_pending{false};
  std::mutex writer_mu;
  reader_slot* slots;

  pcpu_rw_lock_v2() : slots(new reader_slot[N_SLOTS]) {}
  ~pcpu_rw_lock_v2() { delete[] slots; }
  pcpu_rw_lock_v2(const pcpu_rw_lock_v2&) = delete;
  pcpu_rw_lock_v2& operator=(const pcpu_rw_lock_v2&) = delete;

  // Process-wide TLS slot assignment, shared with v1 in spirit (each thread
  // gets a stable slot index in [0, N_SLOTS)). We use a separate counter from
  // v1 because the slot indices are independent — both locks can coexist in
  // the same binary and a thread can use both.
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
    slots[s].count.fetch_add(1, std::memory_order_acq_rel);
    if (writer_pending.load(std::memory_order_acquire)) {
      // Slow path: a writer is queued. Retract our publish so the writer's
      // drain can complete, then wait on the writer_mu queue rather than
      // spinning on writer_pending. The mutex breaks the herd: wake-up is
      // serialised by the mutex's FIFO-ish policy (futex on Linux).
      slots[s].count.fetch_sub(1, std::memory_order_release);
      writer_mu.lock();
      // When we acquire writer_mu, the writer is no longer in its CS (it
      // releases mu in write_unlock) and writer_pending is clear. Re-publish
      // our presence; any subsequent writer must wait for us.
      slots[s].count.fetch_add(1, std::memory_order_acq_rel);
      writer_mu.unlock();
    }
  }

  void read_unlock() noexcept {
    slots[my_slot()].count.fetch_sub(1, std::memory_order_release);
  }

  void write_lock() noexcept {
    writer_mu.lock();
    writer_pending.store(true, std::memory_order_release);
    // Drain in-flight fast-path readers. Slow-path readers are queued on
    // writer_mu (we hold it) and cannot proceed.
    for (int i = 0; i < N_SLOTS; ++i) {
      while (slots[i].count.load(std::memory_order_acquire) != 0) cpu_relax();
    }
  }

  void write_unlock() noexcept {
    writer_pending.store(false, std::memory_order_release);
    writer_mu.unlock();
  }

  // Wormhole shim expects these as aliases for the writer side.
  void lock()   noexcept { write_lock(); }
  void unlock() noexcept { write_unlock(); }

  // Non-blocking try variants.
  bool try_write_lock() noexcept {
    if (!writer_mu.try_lock()) return false;
    writer_pending.store(true, std::memory_order_release);
    for (int i = 0; i < N_SLOTS; ++i) {
      if (slots[i].count.load(std::memory_order_acquire) != 0) {
        writer_pending.store(false, std::memory_order_release);
        writer_mu.unlock();
        return false;
      }
    }
    return true;
  }

  bool try_read_lock() noexcept {
    int s = my_slot();
    slots[s].count.fetch_add(1, std::memory_order_acq_rel);
    if (!writer_pending.load(std::memory_order_acquire)) return true;
    slots[s].count.fetch_sub(1, std::memory_order_release);
    return false;
  }
};
