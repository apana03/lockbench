#pragma once
#include <atomic>
#include <cstdint>
#include "util.hpp"

// Two counters on separate cache lines so they don't interfere
struct ticket_lock {
  alignas(64) std::atomic<std::uint32_t> next{0};
  alignas(64) std::atomic<std::uint32_t> owner{0};

  void lock() noexcept {
    std::uint32_t ticket = next.fetch_add(1, std::memory_order_relaxed);
    while (owner.load(std::memory_order_acquire) != ticket) {
      cpu_relax();
    }
  }

  void unlock() noexcept {
    // just bump the counter so the next thread can go
    std::uint32_t cur = owner.load(std::memory_order_relaxed);
    owner.store(cur + 1, std::memory_order_release);
  }

  // Non-blocking acquire that never burns a queue slot. Succeeds only if
  // the queue is empty (next == owner). Used by wormhole's reader fast
  // path; under sustained writer contention this will fail far more often
  // than rw_lock's try variants. See plan/EXPERIMENT.md.
  bool try_lock() noexcept {
    std::uint32_t cur_owner = owner.load(std::memory_order_relaxed);
    std::uint32_t cur_next  = next.load(std::memory_order_relaxed);
    if (cur_next != cur_owner) return false;  // queue non-empty
    return next.compare_exchange_strong(
        cur_next, cur_owner + 1,
        std::memory_order_acquire, std::memory_order_relaxed);
  }
};
