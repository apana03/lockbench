#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
  #include <immintrin.h>
  inline void cpu_relax() noexcept { _mm_pause(); }
#elif defined(__aarch64__)
  inline void cpu_relax() noexcept { asm volatile("yield" ::: "memory"); }
#else
  inline void cpu_relax() noexcept { std::this_thread::yield(); }
#endif

//Simple spin barrier: all threads wait until "go" is set.
struct start_barrier {
  std::atomic<int> arrived{0};
  std::atomic<bool> go{false};
  int total;

  explicit start_barrier(int n) : total(n) {}

  void arrive_and_wait() {
    arrived.fetch_add(1, std::memory_order_acq_rel);
    while (!go.load(std::memory_order_acquire)) cpu_relax();
  }

  void wait_all_arrived() {
    while (arrived.load(std::memory_order_acquire) < total) cpu_relax();
  }

  void release() {
    go.store(true, std::memory_order_release);
  }
};

// Busy-wait for approximately `ns` (kept deterministic & no syscalls).
inline void busy_wait_ns(std::uint64_t ns) {
  auto start = std::chrono::steady_clock::now();
  while (true) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count();
    if (static_cast<std::uint64_t>(elapsed) >= ns) break;
    cpu_relax();
  }
}