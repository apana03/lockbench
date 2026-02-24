#pragma once
#include <atomic>
#include "util.hpp"

struct tas_lock {
  std::atomic_flag flag = ATOMIC_FLAG_INIT;

  void lock() noexcept {
    while (flag.test_and_set(std::memory_order_acquire)) {
      cpu_relax();
    }
  }
  void unlock() noexcept {
    flag.clear(std::memory_order_release);
  }
};