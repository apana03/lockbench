// lockbench - raw lock/unlock benchmarks
// three modes: mutex (exclusive), rw (reader-writer), rcu (epoch-based)

#include <algorithm>
#include <atomic>
#include <chrono>
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
#include "../include/primitives/occ.hpp"
#include "../include/primitives/rcu.hpp"

struct params {
  std::string lock_name = "tas";
  std::string workload  = "mutex";   // mutex | rw | rcu
  int  threads          = std::max(1u, std::thread::hardware_concurrency());
  int  seconds          = 3;
  int  warmup_seconds   = 1;
  std::uint64_t cs_ns   = 0;         // synthetic work inside critical section
  int  read_pct         = 80;        // read percentage for rw / rcu workloads
};

static void usage() {
  std::cout <<
    "Usage: lockbench [OPTIONS]\n"
    "\n"
    "Options:\n"
    "  --lock <name>     Lock primitive (tas|ttas|cas|ticket|rw|occ|rcu)\n"
    "  --workload <w>    Workload type (mutex|rw|rcu) [default: mutex]\n"
    "  --threads  <N>    Number of worker threads [default: hw_concurrency]\n"
    "  --seconds  <S>    Measurement duration in seconds [default: 3]\n"
    "  --warmup   <S>    Warmup duration in seconds [default: 1]\n"
    "  --cs_ns    <NS>   Busy-wait nanoseconds inside critical section [default: 0]\n"
    "  --read_pct <P>    Read percentage for rw/rcu workloads (0-100) [default: 80]\n";
}

static params parse_args(int argc, char** argv) {
  params p;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) { std::cerr << "Missing value for " << name << "\n"; std::exit(2); }
      return std::string(argv[++i]);
    };

    if      (a == "--lock")     p.lock_name = need("--lock");
    else if (a == "--workload") p.workload  = need("--workload");
    else if (a == "--threads")  p.threads   = std::stoi(need("--threads"));
    else if (a == "--seconds")  p.seconds   = std::stoi(need("--seconds"));
    else if (a == "--warmup")   p.warmup_seconds = std::stoi(need("--warmup"));
    else if (a == "--cs_ns")    p.cs_ns     = std::stoull(need("--cs_ns"));
    else if (a == "--read_pct") p.read_pct  = std::stoi(need("--read_pct"));
    else if (a == "--help" || a == "-h") { usage(); std::exit(0); }
    else { std::cerr << "Unknown arg: " << a << "\n"; std::exit(2); }
  }
  p.threads = std::max(p.threads, 1);
  p.seconds = std::max(p.seconds, 1);
  p.warmup_seconds = std::max(p.warmup_seconds, 0);
  p.read_pct = std::clamp(p.read_pct, 0, 100);
  return p;
}

struct bench_result {
  const char* lock_label;
  const char* workload_label;
  int threads;
  std::uint64_t cs_ns;
  int read_pct;
  double secs;
  std::uint64_t total_ops;
  std::uint64_t read_ops;
  std::uint64_t write_ops;
  std::vector<std::uint64_t> per_thread;
};

static void print_result(const bench_result& r) {
  double ops_s = static_cast<double>(r.total_ops) / r.secs;
  double ns_op = (r.secs * 1e9) / std::max<std::uint64_t>(1, r.total_ops);

  std::cout
    << "lock=" << r.lock_label
    << " workload=" << r.workload_label
    << " threads=" << r.threads
    << " cs_ns=" << r.cs_ns
    << " read_pct=" << r.read_pct
    << " seconds=" << r.secs
    << " total_ops=" << r.total_ops
    << " read_ops=" << r.read_ops
    << " write_ops=" << r.write_ops
    << " ops_s=" << static_cast<std::uint64_t>(ops_s)
    << " ns_op=" << ns_op
    << "\n";

  if (r.threads > 1) {
    std::uint64_t mn = *std::min_element(r.per_thread.begin(), r.per_thread.end());
    std::uint64_t mx = *std::max_element(r.per_thread.begin(), r.per_thread.end());
    double avg = static_cast<double>(r.total_ops) / r.threads;
    double fairness = (mx > 0) ? static_cast<double>(mn) / static_cast<double>(mx) : 1.0;
    std::cout
      << "  fairness: min=" << mn << " max=" << mx
      << " avg=" << avg << " ratio=" << fairness << "\n";
  }
}

// just lock/unlock in a loop
template <class Lock>
static void bench_mutex(const params& p, const char* label) {
  Lock lock;
  std::atomic<bool> stop{false};
  start_barrier barrier(p.threads);

  std::vector<std::uint64_t> counts(p.threads, 0);
  std::vector<std::thread> workers;
  workers.reserve(p.threads);

  for (int t = 0; t < p.threads; ++t) {
    workers.emplace_back([&, t] {
      barrier.arrive_and_wait();
      std::uint64_t local = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        lock.lock();
        if (p.cs_ns) busy_wait_ns(p.cs_ns);
        lock.unlock();
        ++local;
      }
      counts[t] = local;
    });
  }

  barrier.wait_all_arrived();
  barrier.release();

  if (p.warmup_seconds > 0)
    std::this_thread::sleep_for(std::chrono::seconds(p.warmup_seconds));

  for (auto& c : counts) c = 0;

  auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(p.seconds));
  stop.store(true, std::memory_order_relaxed);
  for (auto& th : workers) th.join();
  auto t1 = std::chrono::steady_clock::now();

  double secs = std::chrono::duration<double>(t1 - t0).count();
  std::uint64_t total = 0;
  for (auto c : counts) total += c;

  print_result({label, "mutex", p.threads, p.cs_ns, 100, secs,
                total, 0, total, counts});
}

// rw lock benchmark - readers and writers on a shared counter
static void bench_rw(const params& p, const char* label) {
  rw_lock lock;
  std::atomic<bool> stop{false};
  start_barrier barrier(p.threads);

  alignas(64) std::uint64_t shared_data = 0;

  struct thread_stats { std::uint64_t reads = 0; std::uint64_t writes = 0; };
  std::vector<thread_stats> stats(p.threads);
  std::vector<std::thread> workers;
  workers.reserve(p.threads);

  for (int t = 0; t < p.threads; ++t) {
    workers.emplace_back([&, t] {
      barrier.arrive_and_wait();
      std::mt19937 rng(t + 42);
      std::uniform_int_distribution<int> dist(0, 99);

      while (!stop.load(std::memory_order_relaxed)) {
        if (dist(rng) < p.read_pct) {
          lock.read_lock();
          if (p.cs_ns) busy_wait_ns(p.cs_ns);
          volatile std::uint64_t sink = shared_data; (void)sink;
          lock.read_unlock();
          ++stats[t].reads;
        } else {
          lock.write_lock();
          if (p.cs_ns) busy_wait_ns(p.cs_ns);
          ++shared_data;
          lock.write_unlock();
          ++stats[t].writes;
        }
      }
    });
  }

  barrier.wait_all_arrived();
  barrier.release();

  if (p.warmup_seconds > 0)
    std::this_thread::sleep_for(std::chrono::seconds(p.warmup_seconds));

  for (auto& s : stats) { s.reads = 0; s.writes = 0; }

  auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(p.seconds));
  stop.store(true, std::memory_order_relaxed);
  for (auto& th : workers) th.join();
  auto t1 = std::chrono::steady_clock::now();

  double secs = std::chrono::duration<double>(t1 - t0).count();
  std::uint64_t total_reads = 0, total_writes = 0;
  std::vector<std::uint64_t> per_thread(p.threads);
  for (int t = 0; t < p.threads; ++t) {
    total_reads  += stats[t].reads;
    total_writes += stats[t].writes;
    per_thread[t] = stats[t].reads + stats[t].writes;
  }

  print_result({label, "rw", p.threads, p.cs_ns, p.read_pct, secs,
                total_reads + total_writes, total_reads, total_writes, per_thread});
}

// same thing but with OCC - readers don't take a lock, just validate after
static void bench_occ_rw(const params& p, const char* label) {
  occ_lock lock;
  std::atomic<bool> stop{false};
  start_barrier barrier(p.threads);

  alignas(64) std::uint64_t shared_data = 0;

  struct thread_stats { std::uint64_t reads = 0; std::uint64_t writes = 0; };
  std::vector<thread_stats> stats(p.threads);
  std::vector<std::thread> workers;
  workers.reserve(p.threads);

  for (int t = 0; t < p.threads; ++t) {
    workers.emplace_back([&, t] {
      barrier.arrive_and_wait();
      std::mt19937 rng(t + 42);
      std::uniform_int_distribution<int> dist(0, 99);

      while (!stop.load(std::memory_order_relaxed)) {
        if (dist(rng) < p.read_pct) {
          std::uint64_t val;
          do {
            auto v = lock.read_begin();
            val = shared_data;
            if (p.cs_ns) busy_wait_ns(p.cs_ns);
            if (lock.read_validate(v)) break;
          } while (true);
          volatile std::uint64_t sink = val; (void)sink;
          ++stats[t].reads;
        } else {
          lock.write_lock();
          if (p.cs_ns) busy_wait_ns(p.cs_ns);
          ++shared_data;
          lock.write_unlock();
          ++stats[t].writes;
        }
      }
    });
  }

  barrier.wait_all_arrived();
  barrier.release();

  if (p.warmup_seconds > 0)
    std::this_thread::sleep_for(std::chrono::seconds(p.warmup_seconds));

  for (auto& s : stats) { s.reads = 0; s.writes = 0; }

  auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(p.seconds));
  stop.store(true, std::memory_order_relaxed);
  for (auto& th : workers) th.join();
  auto t1 = std::chrono::steady_clock::now();

  double secs = std::chrono::duration<double>(t1 - t0).count();
  std::uint64_t total_reads = 0, total_writes = 0;
  std::vector<std::uint64_t> per_thread(p.threads);
  for (int t = 0; t < p.threads; ++t) {
    total_reads  += stats[t].reads;
    total_writes += stats[t].writes;
    per_thread[t] = stats[t].reads + stats[t].writes;
  }

  print_result({label, "rw", p.threads, p.cs_ns, p.read_pct, secs,
                total_reads + total_writes, total_reads, total_writes, per_thread});
}

// RCU benchmark - readers announce their epoch, writers swap a pointer and wait
static void bench_rcu(const params& p, const char* label) {
  epoch_rcu rcu(p.threads);
  std::atomic<bool> stop{false};
  start_barrier barrier(p.threads);

  alignas(64) std::atomic<std::uint64_t*> data_ptr{new std::uint64_t(0)};

  struct thread_stats { std::uint64_t reads = 0; std::uint64_t writes = 0; };
  std::vector<thread_stats> stats(p.threads);
  std::vector<std::thread> workers;
  workers.reserve(p.threads);

  // need a lock so only one writer at a time
  cas_lock writer_lock;

  for (int t = 0; t < p.threads; ++t) {
    workers.emplace_back([&, t] {
      barrier.arrive_and_wait();
      std::mt19937 rng(t + 42);
      std::uniform_int_distribution<int> dist(0, 99);

      while (!stop.load(std::memory_order_relaxed)) {
        if (dist(rng) < p.read_pct) {
          rcu.read_lock(t);
          std::uint64_t* ptr = data_ptr.load(std::memory_order_acquire);
          volatile std::uint64_t sink = *ptr; (void)sink;
          if (p.cs_ns) busy_wait_ns(p.cs_ns);
          rcu.read_unlock(t);
          ++stats[t].reads;
        } else {
          writer_lock.lock();
          std::uint64_t* old = data_ptr.load(std::memory_order_relaxed);
          std::uint64_t* fresh = new std::uint64_t(*old + 1);
          data_ptr.store(fresh, std::memory_order_release);
          rcu.synchronize();
          delete old;
          if (p.cs_ns) busy_wait_ns(p.cs_ns);
          writer_lock.unlock();
          ++stats[t].writes;
        }
      }
    });
  }

  barrier.wait_all_arrived();
  barrier.release();

  if (p.warmup_seconds > 0)
    std::this_thread::sleep_for(std::chrono::seconds(p.warmup_seconds));

  for (auto& s : stats) { s.reads = 0; s.writes = 0; }

  auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(p.seconds));
  stop.store(true, std::memory_order_relaxed);
  for (auto& th : workers) th.join();
  auto t1 = std::chrono::steady_clock::now();

  delete data_ptr.load();

  double secs = std::chrono::duration<double>(t1 - t0).count();
  std::uint64_t total_reads = 0, total_writes = 0;
  std::vector<std::uint64_t> per_thread(p.threads);
  for (int t = 0; t < p.threads; ++t) {
    total_reads  += stats[t].reads;
    total_writes += stats[t].writes;
    per_thread[t] = stats[t].reads + stats[t].writes;
  }

  print_result({label, "rcu", p.threads, p.cs_ns, p.read_pct, secs,
                total_reads + total_writes, total_reads, total_writes, per_thread});
}

int main(int argc, char** argv) {
  params p = parse_args(argc, argv);

  if (p.workload == "mutex") {
    if      (p.lock_name == "tas")    bench_mutex<tas_lock>(p, "tas");
    else if (p.lock_name == "ttas")   bench_mutex<ttas_lock>(p, "ttas");
    else if (p.lock_name == "cas")    bench_mutex<cas_lock>(p, "cas");
    else if (p.lock_name == "ticket") bench_mutex<ticket_lock>(p, "ticket");
    else if (p.lock_name == "rw")     bench_mutex<rw_lock>(p, "rw");
    else if (p.lock_name == "occ")    bench_mutex<occ_lock>(p, "occ");
    else {
      std::cerr << "Unsupported --lock " << p.lock_name
                << " for mutex workload (use tas|ttas|cas|ticket|rw|occ)\n";
      return 2;
    }
  } else if (p.workload == "rw") {
    if      (p.lock_name == "rw")  bench_rw(p, "rw");
    else if (p.lock_name == "occ") bench_occ_rw(p, "occ");
    else {
      std::cerr << "Unsupported --lock " << p.lock_name
                << " for rw workload (use rw|occ)\n";
      return 2;
    }
  } else if (p.workload == "rcu") {
    if (p.lock_name == "rcu") bench_rcu(p, "rcu");
    else {
      std::cerr << "Unsupported --lock " << p.lock_name
                << " for rcu workload (use rcu)\n";
      return 2;
    }
  } else {
    std::cerr << "Unknown --workload " << p.workload
              << " (use mutex|rw|rcu)\n";
    return 2;
  }

  return 0;
}
