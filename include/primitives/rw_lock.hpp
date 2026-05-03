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

  void lock() noexcept { write_lock(); }
  void unlock() noexcept { write_unlock(); }

  // Non-blocking variants used by wormhole's reader/writer fast paths.
  bool try_write_lock() noexcept {
    std::int32_t expected = 0;
    return state.compare_exchange_strong(
        expected, -1, std::memory_order_acquire, std::memory_order_relaxed);
  }
  bool try_read_lock() noexcept {
    std::int32_t s = state.load(std::memory_order_relaxed);
    if (s < 0) return false;
    return state.compare_exchange_strong(
        s, s + 1, std::memory_order_acquire, std::memory_order_relaxed);
  }
};
