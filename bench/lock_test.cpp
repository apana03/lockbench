// locktest - correctness tests for lock implementations
// verifies mutual exclusion by having threads do non-atomic increments
// inside a critical section, then checking the final count

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../include/primitives/util.hpp"
#include "../include/primitives/tas_lock.hpp"
#include "../include/primitives/ttas_lock.hpp"
#include "../include/primitives/cas_lock.hpp"
#include "../include/primitives/ticket_lock.hpp"
#include "../include/primitives/rw_lock.hpp"
#include "../include/primitives/occ.hpp"

struct params {
  int threads         = std::max(1u, std::thread::hardware_concurrency());
  std::uint64_t loops = 100000;
  std::string lock    = "";  // empty = run all
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
    else if (a == "--help" || a == "-h") { usage(); std::exit(0); }
    else { std::cerr << "Unknown arg: " << a << "\n"; std::exit(2); }
  }
  p.threads = std::max(p.threads, 1);
  return p;
}

// test mutual exclusion via non-atomic increment
template <class Lock>
bool test_mutex(const char* name, int threads, std::uint64_t loops) {
  Lock lock;
  std::uint64_t counter = 0;
  start_barrier barrier(threads);
  std::vector<std::thread> workers;
  workers.reserve(threads);

  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&] {
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
bool test_rw_write(const char* name, int threads, std::uint64_t loops) {
  rw_lock lock;
  std::uint64_t counter = 0;
  start_barrier barrier(threads);
  std::vector<std::thread> workers;
  workers.reserve(threads);

  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&] {
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

// test occ_lock in exclusive (write) mode
bool test_occ_write(const char* name, int threads, std::uint64_t loops) {
  occ_lock lock;
  std::uint64_t counter = 0;
  start_barrier barrier(threads);
  std::vector<std::thread> workers;
  workers.reserve(threads);

  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&] {
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

  run("tas",    [&] { return test_mutex<tas_lock>("tas_lock", p.threads, p.loops); });
  run("ttas",   [&] { return test_mutex<ttas_lock>("ttas_lock", p.threads, p.loops); });
  run("cas",    [&] { return test_mutex<cas_lock>("cas_lock", p.threads, p.loops); });
  run("ticket", [&] { return test_mutex<ticket_lock>("ticket_lock", p.threads, p.loops); });
  run("rw",     [&] { return test_rw_write("rw_lock (write)", p.threads, p.loops); });
  run("occ",    [&] { return test_occ_write("occ_lock (write)", p.threads, p.loops); });

  std::cout << "\n" << (all_ok ? "ALL PASSED" : "SOME FAILED") << "\n";
  return all_ok ? 0 : 1;
}
