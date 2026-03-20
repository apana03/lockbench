#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
  #include <immintrin.h>
  inline void cpu_relax() noexcept { _mm_pause(); }
#elif defined(__aarch64__)
  inline void cpu_relax() noexcept { asm volatile("yield" ::: "memory"); }
#else
  inline void cpu_relax() noexcept { std::this_thread::yield(); }
#endif

// bias threads toward performance cores on macOS (QoS hint)
#if defined(__APPLE__)
  #include <pthread.h>
  inline void set_thread_high_priority() noexcept {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
  }
#else
  inline void set_thread_high_priority() noexcept {}
#endif

// pin calling thread to a specific core (Linux only)
#if defined(__linux__)
  #include <sched.h>
  inline bool set_thread_affinity(int core_id) noexcept {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0;
  }
#else
  inline bool set_thread_affinity(int) noexcept { return false; }
#endif

// call at the start of each worker thread before the barrier
inline void setup_worker_thread([[maybe_unused]] int thread_id,
                                [[maybe_unused]] bool pin) noexcept {
  set_thread_high_priority();
#if defined(__linux__)
  if (pin) set_thread_affinity(thread_id);
#endif
}

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

// format a double with comma as decimal separator for CSV output
inline std::string fmt_double(double v) {
  std::string s = std::to_string(v);
  for (auto& c : s) if (c == '.') c = ',';
  return s;
}

// do 'iters' iterations of dummy work to simulate a critical section
// compiler barrier prevents the loop from being optimized away
inline void busy_work(std::uint64_t iters) {
  for (std::uint64_t i = 0; i < iters; ++i) {
    asm volatile("" ::: "memory");
  }
}