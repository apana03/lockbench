// locktest - correctness tests for lock implementations
// verifies mutual exclusion by having threads do non-atomic increments
// inside a critical section, then checking the final count

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../include/primitives/util.hpp"
#include "../include/primitives/tas_lock.hpp"
#include "../include/primitives/ttas_lock.hpp"
#include "../include/primitives/cas_lock.hpp"
#include "../include/primitives/ticket_lock.hpp"
#include "../include/primitives/rw_lock.hpp"
#include "../include/primitives/pcpu_rw_lock.hpp"
#include "../include/primitives/pcpu_rw_lock_v2.hpp"
#include "../include/primitives/occ.hpp"

struct params {
  int threads         = std::max(1u, std::thread::hardware_concurrency());
  std::uint64_t loops = 100000;
  std::string lock    = "";  // empty = run all
  bool pin            = false;
};

static void usage() {
  std::cout <<
    "Usage: locktest [OPTIONS]\n"
    "\n"
    "Options:\n"
    "  --threads <N>   Number of worker threads [default: hw_concurrency]\n"
    "  --loops   <N>   Iterations per thread [default: 100000]\n"
    "  --lock    <name> Run a specific lock (tas|ttas|cas|ticket|rw|occ)\n";
}

static params parse_args(int argc, char** argv) {
  params p;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) { std::cerr << "Missing value for " << name << "\n"; std::exit(2); }
      return std::string(argv[++i]);
    };

    if      (a == "--threads") p.threads = std::stoi(need("--threads"));
    else if (a == "--loops")   p.loops   = std::stoull(need("--loops"));
    else if (a == "--lock")    p.lock    = need("--lock");
    else if (a == "--pin")     p.pin     = true;
    else if (a == "--help" || a == "-h") { usage(); std::exit(0); }
    else { std::cerr << "Unknown arg: " << a << "\n"; std::exit(2); }
  }
  p.threads = std::max(p.threads, 1);
  return p;
}

// test mutual exclusion via non-atomic increment
template <class Lock>
bool test_mutex(const char* name, int threads, std::uint64_t loops, bool pin) {
  Lock lock;
  std::uint64_t counter = 0;
  start_barrier barrier(threads);
  std::vector<std::thread> workers;
  workers.reserve(threads);

  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      setup_worker_thread(t, pin);
      barrier.arrive_and_wait();
      for (std::uint64_t i = 0; i < loops; ++i) {
        lock.lock();
        std::uint64_t tmp = counter;
        counter = tmp + 1;
        lock.unlock();
      }
    });
  }

  barrier.wait_all_arrived();
  barrier.release();
  for (auto& th : workers) th.join();

  std::uint64_t expected = loops * threads;
  bool ok = (counter == expected);
  std::cout << name << ": " << (ok ? "PASS" : "FAIL")
            << " (expected=" << expected << " got=" << counter << ")\n";
  return ok;
}

// test rw_lock in exclusive (write) mode
bool test_rw_write(const char* name, int threads, std::uint64_t loops, bool pin) {
  rw_lock lock;
  std::uint64_t counter = 0;
  start_barrier barrier(threads);
  std::vector<std::thread> workers;
  workers.reserve(threads);

  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      setup_worker_thread(t, pin);
      barrier.arrive_and_wait();
      for (std::uint64_t i = 0; i < loops; ++i) {
        lock.write_lock();
        std::uint64_t tmp = counter;
        counter = tmp + 1;
        lock.write_unlock();
      }
    });
  }

  barrier.wait_all_arrived();
  barrier.release();
  for (auto& th : workers) th.join();

  std::uint64_t expected = loops * threads;
  bool ok = (counter == expected);
  std::cout << name << ": " << (ok ? "PASS" : "FAIL")
            << " (expected=" << expected << " got=" << counter << ")\n";
  return ok;
}

// test pcpu_rw_lock — write-side mutual exclusion (covers slot drain logic)
template <class PcpuRwLock>
bool test_pcpu_rw_write(const char* name, int threads, std::uint64_t loops, bool pin) {
  PcpuRwLock lock;
  std::uint64_t counter = 0;
  start_barrier barrier(threads);
  std::vector<std::thread> workers;
  workers.reserve(threads);

  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      setup_worker_thread(t, pin);
      barrier.arrive_and_wait();
      for (std::uint64_t i = 0; i < loops; ++i) {
        lock.write_lock();
        std::uint64_t tmp = counter;
        counter = tmp + 1;
        lock.write_unlock();
      }
    });
  }

  barrier.wait_all_arrived();
  barrier.release();
  for (auto& th : workers) th.join();

  std::uint64_t expected = loops * threads;
  bool ok = (counter == expected);
  std::cout << name << ": " << (ok ? "PASS" : "FAIL")
            << " (expected=" << expected << " got=" << counter << ")\n";
  return ok;
}

// test pcpu_rw_lock — readers must NEVER observe a partial write.
// Each reader reads a 16-byte payload twice; if the lock works, the two
// reads must agree (since writers update the payload atomically under the
// write lock). A counter-style invariant: payload.first == payload.second.
template <class PcpuRwLock>
bool test_pcpu_rw_mixed(const char* name, int threads, std::uint64_t loops, bool pin,
                       int read_pct = 80) {
  PcpuRwLock lock;
  struct alignas(16) pair_t { std::uint64_t a; std::uint64_t b; };
  pair_t shared{0, 0};
  std::atomic<bool> consistency_violated{false};
  start_barrier barrier(threads);
  std::vector<std::thread> workers;
  workers.reserve(threads);

  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      setup_worker_thread(t, pin);
      barrier.arrive_and_wait();
      std::mt19937 rng(t + 42);
      std::uniform_int_distribution<int> dist(0, 99);
      for (std::uint64_t i = 0; i < loops; ++i) {
        if (dist(rng) < read_pct) {
          lock.read_lock();
          std::uint64_t a = shared.a;
          std::uint64_t b = shared.b;
          if (a != b) consistency_violated.store(true, std::memory_order_relaxed);
          lock.read_unlock();
        } else {
          lock.write_lock();
          std::uint64_t v = shared.a + 1;
          shared.a = v;
          shared.b = v;
          lock.write_unlock();
        }
      }
    });
  }

  barrier.wait_all_arrived();
  barrier.release();
  for (auto& th : workers) th.join();

  bool ok = !consistency_violated.load() && shared.a == shared.b;
  std::cout << name << ": " << (ok ? "PASS" : "FAIL")
            << " (final a=" << shared.a << " b=" << shared.b
            << " torn=" << (consistency_violated.load() ? "yes" : "no") << ")\n";
  return ok;
}

// test occ_lock in exclusive (write) mode
bool test_occ_write(const char* name, int threads, std::uint64_t loops, bool pin) {
  occ_lock lock;
  std::uint64_t counter = 0;
  start_barrier barrier(threads);
  std::vector<std::thread> workers;
  workers.reserve(threads);

  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      setup_worker_thread(t, pin);
      barrier.arrive_and_wait();
      for (std::uint64_t i = 0; i < loops; ++i) {
        lock.write_lock();
        std::uint64_t tmp = counter;
        counter = tmp + 1;
        lock.write_unlock();
      }
    });
  }

  barrier.wait_all_arrived();
  barrier.release();
  for (auto& th : workers) th.join();

  std::uint64_t expected = loops * threads;
  bool ok = (counter == expected);
  std::cout << name << ": " << (ok ? "PASS" : "FAIL")
            << " (expected=" << expected << " got=" << counter << ")\n";
  return ok;
}

int main(int argc, char** argv) {
  params p = parse_args(argc, argv);

  std::cout << "locktest: threads=" << p.threads
            << " loops=" << p.loops << "\n\n";

  bool all_ok = true;
  auto run = [&](const std::string& name, auto fn) {
    if (p.lock.empty() || p.lock == name)
      all_ok &= fn();
  };

  run("tas",    [&] { return test_mutex<tas_lock>("tas_lock", p.threads, p.loops, p.pin); });
  run("ttas",   [&] { return test_mutex<ttas_lock>("ttas_lock", p.threads, p.loops, p.pin); });
  run("cas",    [&] { return test_mutex<cas_lock>("cas_lock", p.threads, p.loops, p.pin); });
  run("ticket", [&] { return test_mutex<ticket_lock>("ticket_lock", p.threads, p.loops, p.pin); });
  run("rw",     [&] { return test_rw_write("rw_lock (write)", p.threads, p.loops, p.pin); });
  run("pcpu-rw",[&] { return test_pcpu_rw_write<pcpu_rw_lock>("pcpu_rw_lock (write)", p.threads, p.loops, p.pin)
                            && test_pcpu_rw_mixed<pcpu_rw_lock>("pcpu_rw_lock (mixed)", p.threads, p.loops, p.pin); });
  run("pcpu-rw-v2",[&] { return test_pcpu_rw_write<pcpu_rw_lock_v2>("pcpu_rw_lock_v2 (write)", p.threads, p.loops, p.pin)
                            && test_pcpu_rw_mixed<pcpu_rw_lock_v2>("pcpu_rw_lock_v2 (mixed)", p.threads, p.loops, p.pin); });
  run("occ",    [&] { return test_occ_write("occ_lock (write)", p.threads, p.loops, p.pin); });

  std::cout << "\n" << (all_ok ? "ALL PASSED" : "SOME FAILED") << "\n";
  return all_ok ? 0 : 1;
}
