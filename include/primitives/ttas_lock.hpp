#pragma once
#include <atomic>
#include "util.hpp"

struct ttas_lock {
  std::atomic<bool> state{false};

  void lock() noexcept {
    for (;;) {
      // spin on reads first so we don't spam expensive atomic writes
      while (state.load(std::memory_order_relaxed)) cpu_relax();

      // now try to actually grab it
      if (!state.exchange(true, std::memory_order_acquire)) return;
    }
  }

  void unlock() noexcept {
    state.store(false, std::memory_order_release);
  }
};