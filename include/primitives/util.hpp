#pragma once
#include <atomic>
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

// spin barrier - threads wait here until everyone is ready
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

// do 'iters' iterations of dummy work to simulate a critical section
// compiler barrier prevents the loop from being optimized away
inline void busy_work(std::uint64_t iters) {
  for (std::uint64_t i = 0; i < iters; ++i) {
    asm volatile("" ::: "memory");
  }
}