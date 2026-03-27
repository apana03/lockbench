#pragma once
#include <atomic>
#include <cstdint>
#include "util.hpp"

// OCC lock (basically a seqlock)
// even version = safe to read, odd = someone is writing
// readers just check the version before and after, retry if it changed
struct occ_lock {
  alignas(64) std::atomic<std::uint64_t> version{0};

  void write_lock() noexcept {
    for (;;) {
      std::uint64_t v = version.load(std::memory_order_relaxed);
      if (v & 1) { cpu_relax(); continue; } // odd = another writer, wait
      // try to flip to odd (marks write in progress)
      if (version.compare_exchange_weak(v, v + 1, std::memory_order_acquire,
                                        std::memory_order_relaxed))
        return;
    }
  }

  void write_unlock() noexcept {
    // bump back to even = done writing
    version.fetch_add(1, std::memory_order_release);
  }

  // grab the version before reading, spin if a write is happening
  std::uint64_t read_begin() const noexcept {
    for (;;) {
      std::uint64_t v = version.load(std::memory_order_acquire);
      if (!(v & 1)) return v;
      cpu_relax();
    }
  }

  // check if version is still the same - if not, a write happened and we need to retry
  // the acquire fence ensures the data load completes before we re-check the version
  // the acquire load ensures we observe the writer's release store (write_unlock)
  bool read_validate(std::uint64_t start_version) const noexcept {
    std::atomic_thread_fence(std::memory_order_acquire);
    return version.load(std::memory_order_acquire) == start_version;
  }

  // for the mutex benchmarks
  void lock() noexcept { write_lock(); }
  void unlock() noexcept { write_unlock(); }
};
