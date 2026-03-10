#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>
#include "util.hpp"

// Simple epoch-based RCU
// readers announce which epoch they're in, writers bump the epoch
// and wait for all old readers to finish before freeing stuff
// (you have to pass in thread IDs manually, not great but works for benchmarks)
struct epoch_rcu {
  static constexpr std::uint64_t INACTIVE = UINT64_MAX;

  alignas(64) std::atomic<std::uint64_t> global_epoch{0};

  // each thread gets its own slot (padded so they don't share cache lines)
  struct alignas(64) slot {
    std::atomic<std::uint64_t> epoch{INACTIVE};
  };

  std::vector<slot> slots;
  int max_threads;

  explicit epoch_rcu(int n) : slots(n), max_threads(n) {}

  std::uint64_t read_lock(int tid) noexcept {
    std::uint64_t e = global_epoch.load(std::memory_order_acquire);
    slots[tid].epoch.store(e, std::memory_order_release);
    return e;
  }

  void read_unlock(int tid) noexcept {
    slots[tid].epoch.store(INACTIVE, std::memory_order_release);
  }

  // writer calls this to wait until all old readers are done
  void synchronize() noexcept {
    std::uint64_t new_epoch = global_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    // wait for every thread to either leave or move to the new epoch
    for (int i = 0; i < max_threads; ++i) {
      while (true) {
        std::uint64_t re = slots[i].epoch.load(std::memory_order_acquire);
        if (re == INACTIVE || re >= new_epoch) break;
        cpu_relax();
      }
    }
  }
};
