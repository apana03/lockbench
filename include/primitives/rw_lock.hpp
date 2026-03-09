#pragma once
#include <atomic>
#include <cstdint>
#include "util.hpp"

// Spinning reader-writer lock
// state >= 0 means that many readers are in, state == -1 means writer has it
// note: writers can starve if readers keep coming in
struct rw_lock {
  std::atomic<std::int32_t> state{0};

  void read_lock() noexcept {
    for (;;) {
      std::int32_t s = state.load(std::memory_order_relaxed);
      if (s >= 0) {
        if (state.compare_exchange_weak(s, s + 1, std::memory_order_acquire,
                                        std::memory_order_relaxed))
          return;
      } else {
        cpu_relax();
      }
    }
  }

  void read_unlock() noexcept {
    state.fetch_sub(1, std::memory_order_release);
  }

  void write_lock() noexcept {
    for (;;) {
      std::int32_t expected = 0;
      if (state.compare_exchange_weak(expected, -1, std::memory_order_acquire,
                                      std::memory_order_relaxed))
        return;
      cpu_relax();
    }
  }

  void write_unlock() noexcept {
    state.store(0, std::memory_order_release);
  }

  // so we can use it in the same benchmarks as the other locks
  void lock() noexcept { write_lock(); }
  void unlock() noexcept { write_unlock(); }
};
