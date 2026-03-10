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
};
