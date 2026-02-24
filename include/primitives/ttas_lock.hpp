#pragma once
#include <atomic>
#include "util.hpp"

struct ttas_lock {
  std::atomic<bool> state{false};

  void lock() noexcept {
    for (;;) {
      // First: spin-read (shared state) to reduce RMW traffic.
      while (state.load(std::memory_order_relaxed)) cpu_relax();

      // Then: try to acquire with an exchange (RMW).
      if (!state.exchange(true, std::memory_order_acquire)) return;
    }
  }

  void unlock() noexcept {
    state.store(false, std::memory_order_release);
  }
};