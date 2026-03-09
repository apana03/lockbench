#pragma once
#include <atomic>
#include "util.hpp"

// CAS spinlock - same idea as TAS but we use compare_exchange instead
struct cas_lock {
  std::atomic<bool> state{false};

  void lock() noexcept {
    for (;;) {
      bool expected = false;
      if (state.compare_exchange_weak(expected, true, std::memory_order_acquire,
                                      std::memory_order_relaxed))
        return;
      // same trick as TTAS - just spin on reads until it looks free
      while (state.load(std::memory_order_relaxed)) cpu_relax();
    }
  }

  void unlock() noexcept {
    state.store(false, std::memory_order_release);
  }
};
