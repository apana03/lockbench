// Main benchmark entrypoint.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../include/primitives/util.hpp"
#include "../include/primitives/tas_lock.hpp"
#include "../include/primitives/ttas_lock.hpp"

struct params {
  std::string lock_name = "tas";
  int threads = std::max(1u, std::thread::hardware_concurrency());
  int seconds = 3;
  int warmup_seconds = 1;
  std::uint64_t cs_ns = 0; // synthetic work inside critical section
};

static params parse_args(int argc, char** argv) {
  params p;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* name) {
      if (i + 1 >= argc) { std::cerr << "Missing value for " << name << "\n"; std::exit(2); }
      return std::string(argv[++i]);
    };

    if (a == "--lock") p.lock_name = need("--lock");
    else if (a == "--threads") p.threads = std::stoi(need("--threads"));
    else if (a == "--seconds") p.seconds = std::stoi(need("--seconds"));
    else if (a == "--warmup") p.warmup_seconds = std::stoi(need("--warmup"));
    else if (a == "--cs_ns") p.cs_ns = static_cast<std::uint64_t>(std::stoull(need("--cs_ns")));
    else if (a == "--help" || a == "-h") {
      std::cout <<
        "Usage: lockbench [--lock tas|ttas] [--threads N] [--seconds S] [--warmup S] [--cs_ns NS]\n";
      std::exit(0);
    } else {
      std::cerr << "Unknown arg: " << a << "\n";
      std::exit(2);
    }
  }
  if (p.threads <= 0) p.threads = 1;
  if (p.seconds <= 0) p.seconds = 1;
  if (p.warmup_seconds < 0) p.warmup_seconds = 0;
  return p;
}

template <class Lock>
static void run_bench(const params& p, const char* lock_label) {
  Lock lock;
  std::atomic<bool> stop{false};

  start_barrier barrier(p.threads);

  std::vector<std::uint64_t> counts(p.threads, 0);
  std::vector<std::thread> workers;
  workers.reserve(p.threads);

  auto worker = [&](int tid) {
    barrier.arrive_and_wait();
    std::uint64_t local = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      lock.lock();
      if (p.cs_ns) busy_wait_ns(p.cs_ns);
      lock.unlock();
      ++local;
    }
    counts[tid] = local;
  };

  for (int t = 0; t < p.threads; ++t) workers.emplace_back(worker, t);

  // Ensure all threads are ready before starting warmup.
  barrier.wait_all_arrived();
  barrier.release();

  // Warmup
  if (p.warmup_seconds > 0) {
    std::this_thread::sleep_for(std::chrono::seconds(p.warmup_seconds));
  }

  // Measure
  auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(p.seconds));
  stop.store(true, std::memory_order_relaxed);
  for (auto& th : workers) th.join();
  auto t1 = std::chrono::steady_clock::now();

  double secs = std::chrono::duration<double>(t1 - t0).count();
  std::uint64_t total = 0;
  for (auto c : counts) total += c;

  double ops_s = static_cast<double>(total) / secs;
  double ns_op = (secs * 1e9) / std::max<std::uint64_t>(1, total);

  std::cout
    << "lock=" << lock_label
    << ", threads=" << p.threads
    << ", cs_ns=" << p.cs_ns
    << ", seconds=" << p.seconds
    << ", ops_s=" << ops_s
    << ", ns_op=" << ns_op
    << "\n";
}

int main(int argc, char** argv) {
  params p = parse_args(argc, argv);

  if (p.lock_name == "tas") {
    run_bench<tas_lock>(p, "tas");
  } else if (p.lock_name == "ttas") {
    run_bench<ttas_lock>(p, "ttas");
  } else {
    std::cerr << "Unsupported --lock " << p.lock_name << " (use tas|ttas)\n";
    return 2;
  }
  return 0;
}
