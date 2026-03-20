// arraybench - benchmark lock contention on an array of locks
// each thread picks a random lock index, acquires it, does optional work, releases
// rw/occ variants use read_lock/write_lock split controlled by --read_pct

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>

#include "../include/primitives/util.hpp"
#include "../include/primitives/tas_lock.hpp"
#include "../include/primitives/ttas_lock.hpp"
#include "../include/primitives/cas_lock.hpp"
#include "../include/primitives/ticket_lock.hpp"
#include "../include/primitives/rw_lock.hpp"
#include "../include/primitives/occ.hpp"

struct params {
  std::string lock_name  = "ttas";
  int    threads         = std::max(1u, std::thread::hardware_concurrency());
  int    seconds         = 3;
  int    warmup_seconds  = 1;
  std::size_t num_locks  = 64;
  std::uint64_t cs_work  = 0;          // busy_work iterations inside CS
  int    read_pct        = 80;         // read/write split for rw/occ locks
  std::string csv_file;
  bool pin              = false;     // pin threads to cores (Linux only)
};

static void usage() {
  std::cout <<
    "Usage: arraybench [OPTIONS]\n"
    "\n"
    "Options:\n"
    "  --lock <name>       Lock primitive (tas|ttas|cas|ticket|rw|occ)\n"
    "  --threads <N>       Worker threads [default: hw_concurrency]\n"
    "  --seconds <S>       Measurement seconds [default: 3]\n"
    "  --warmup <S>        Warmup seconds [default: 1]\n"
    "  --num_locks <N>     Number of locks in the array [default: 64]\n"
    "  --cs_work <N>       Busy-work loop iterations inside critical section [default: 0]\n"
    "  --read_pct <P>      Read percentage for rw/occ locks (0-100) [default: 80]\n"
    "  --csv <file>        Append results as CSV to file\n";
}

static params parse_args(int argc, char** argv) {
  params p;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) { std::cerr << "Missing value for " << name << "\n"; std::exit(2); }
      return std::string(argv[++i]);
    };
    if      (a == "--lock")       p.lock_name     = need("--lock");
    else if (a == "--threads")    p.threads       = std::stoi(need("--threads"));
    else if (a == "--seconds")    p.seconds       = std::stoi(need("--seconds"));
    else if (a == "--warmup")     p.warmup_seconds = std::stoi(need("--warmup"));
    else if (a == "--num_locks")  p.num_locks     = std::stoull(need("--num_locks"));
    else if (a == "--cs_work")    p.cs_work       = std::stoull(need("--cs_work"));
    else if (a == "--read_pct")   p.read_pct      = std::stoi(need("--read_pct"));
    else if (a == "--csv")        p.csv_file      = need("--csv");
    else if (a == "--pin")        p.pin           = true;
    else if (a == "--help" || a == "-h") { usage(); std::exit(0); }
    else { std::cerr << "Unknown arg: " << a << "\n"; std::exit(2); }
  }
  p.threads = std::max(p.threads, 1);
  p.seconds = std::max(p.seconds, 1);
  p.warmup_seconds = std::max(p.warmup_seconds, 0);
  p.read_pct = std::clamp(p.read_pct, 0, 100);
  p.num_locks = std::max<std::size_t>(p.num_locks, 1);
  return p;
}

static void csv_append(const std::string& path, const std::string& header,
                       const std::string& row) {
  bool write_header = false;
  struct stat st;
  if (stat(path.c_str(), &st) != 0 || st.st_size == 0)
    write_header = true;
  std::ofstream f(path, std::ios::app);
  if (write_header) f << header << "\n";
  f << row << "\n";
}

static void print_result(const char* lock_label, const params& p, double secs,
                          std::uint64_t total_ops, std::uint64_t read_ops,
                          std::uint64_t write_ops,
                          const std::vector<std::uint64_t>& per_thread) {
  double ops_s = static_cast<double>(total_ops) / secs;
  double ns_op = (secs * 1e9) / std::max<std::uint64_t>(1, total_ops);

  std::cout
    << "lock=" << lock_label
    << " threads=" << p.threads
    << " num_locks=" << p.num_locks
    << " cs_work=" << p.cs_work
    << " read_pct=" << p.read_pct
    << " seconds=" << secs
    << " total_ops=" << total_ops
    << " read_ops=" << read_ops
    << " write_ops=" << write_ops
    << " ops_s=" << static_cast<std::uint64_t>(ops_s)
    << " ns_op=" << ns_op
    << "\n";

  std::uint64_t mn = 0, mx = 0;
  double fairness = 1.0;
  if (p.threads > 1) {
    mn = *std::min_element(per_thread.begin(), per_thread.end());
    mx = *std::max_element(per_thread.begin(), per_thread.end());
    fairness = (mx > 0) ? static_cast<double>(mn) / static_cast<double>(mx) : 1.0;
    std::cout
      << "  fairness: min=" << mn << " max=" << mx
      << " ratio=" << fairness << "\n";
  }

  if (!p.csv_file.empty()) {
    std::string header = "lock;threads;num_locks;cs_work;read_pct;total_ops;read_ops;write_ops;ops_s;ns_op;fairness_min;fairness_max;fairness_ratio";
    std::ostringstream row;
    row << lock_label << ";" << p.threads << ";" << p.num_locks << ";"
        << p.cs_work << ";" << p.read_pct << ";"
        << total_ops << ";" << read_ops << ";" << write_ops << ";"
        << static_cast<std::uint64_t>(ops_s) << ";" << fmt_double(ns_op) << ";"
        << mn << ";" << mx << ";" << fmt_double(fairness);
    csv_append(p.csv_file, header, row.str());
  }
}

// exclusive lock benchmark - just lock/unlock on random lock indices
template <class Lock>
static void bench_lock_array(const params& p, const char* label) {
  struct alignas(64) padded_lock { Lock lock; };
  std::vector<padded_lock> locks(p.num_locks);

  std::atomic<bool> stop{false};
  std::atomic<bool> measuring{false};
  start_barrier barrier(p.threads);

  std::vector<std::uint64_t> counts(p.threads, 0);
  std::vector<std::thread> workers;
  workers.reserve(p.threads);

  for (int t = 0; t < p.threads; ++t) {
    workers.emplace_back([&, t] {
      setup_worker_thread(t, p.pin);
      barrier.arrive_and_wait();
      std::mt19937_64 rng(t + 42);
      std::uniform_int_distribution<std::size_t> idx_dist(0, p.num_locks - 1);
      std::uint64_t local = 0;

      while (!stop.load(std::memory_order_relaxed)) {
        std::size_t idx = idx_dist(rng);
        locks[idx].lock.lock();
        if (p.cs_work) busy_work(p.cs_work);
        locks[idx].lock.unlock();
        if (measuring.load(std::memory_order_relaxed)) ++local;
      }
      counts[t] = local;
    });
  }

  barrier.wait_all_arrived();
  barrier.release();

  if (p.warmup_seconds > 0)
    std::this_thread::sleep_for(std::chrono::seconds(p.warmup_seconds));

  measuring.store(true, std::memory_order_relaxed);
  auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(p.seconds));
  stop.store(true, std::memory_order_relaxed);
  for (auto& th : workers) th.join();
  auto t1 = std::chrono::steady_clock::now();

  double secs = std::chrono::duration<double>(t1 - t0).count();
  std::uint64_t total = 0;
  for (auto c : counts) total += c;

  print_result(label, p, secs, total, 0, total, counts);
}

// rw lock benchmark - readers use read_lock, writers use write_lock
static void bench_lock_array_rw(const params& p, const char* label) {
  struct alignas(64) padded_lock { rw_lock lock; };
  std::vector<padded_lock> locks(p.num_locks);

  std::atomic<bool> stop{false};
  std::atomic<bool> measuring{false};
  start_barrier barrier(p.threads);

  struct alignas(64) thread_stats { std::uint64_t reads = 0; std::uint64_t writes = 0; };
  std::vector<thread_stats> stats(p.threads);
  std::vector<std::thread> workers;
  workers.reserve(p.threads);

  for (int t = 0; t < p.threads; ++t) {
    workers.emplace_back([&, t] {
      setup_worker_thread(t, p.pin);
      barrier.arrive_and_wait();
      std::mt19937_64 rng(t + 42);
      std::uniform_int_distribution<std::size_t> idx_dist(0, p.num_locks - 1);
      std::uniform_int_distribution<int> op_dist(0, 99);
      std::uint64_t local_reads = 0, local_writes = 0;

      while (!stop.load(std::memory_order_relaxed)) {
        std::size_t idx = idx_dist(rng);
        if (op_dist(rng) < p.read_pct) {
          locks[idx].lock.read_lock();
          if (p.cs_work) busy_work(p.cs_work);
          locks[idx].lock.read_unlock();
          if (measuring.load(std::memory_order_relaxed)) ++local_reads;
        } else {
          locks[idx].lock.write_lock();
          if (p.cs_work) busy_work(p.cs_work);
          locks[idx].lock.write_unlock();
          if (measuring.load(std::memory_order_relaxed)) ++local_writes;
        }
      }

      stats[t].reads  = local_reads;
      stats[t].writes = local_writes;
    });
  }

  barrier.wait_all_arrived();
  barrier.release();

  if (p.warmup_seconds > 0)
    std::this_thread::sleep_for(std::chrono::seconds(p.warmup_seconds));

  measuring.store(true, std::memory_order_relaxed);
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

  print_result(label, p, secs, total_reads + total_writes,
               total_reads, total_writes, per_thread);
}

// occ lock benchmark - readers use optimistic read_begin/read_validate
static void bench_lock_array_occ(const params& p, const char* label) {
  struct alignas(64) padded_lock { occ_lock lock; };
  std::vector<padded_lock> locks(p.num_locks);

  std::atomic<bool> stop{false};
  std::atomic<bool> measuring{false};
  start_barrier barrier(p.threads);

  struct alignas(64) thread_stats { std::uint64_t reads = 0; std::uint64_t writes = 0; };
  std::vector<thread_stats> stats(p.threads);
  std::vector<std::thread> workers;
  workers.reserve(p.threads);

  for (int t = 0; t < p.threads; ++t) {
    workers.emplace_back([&, t] {
      setup_worker_thread(t, p.pin);
      barrier.arrive_and_wait();
      std::mt19937_64 rng(t + 42);
      std::uniform_int_distribution<std::size_t> idx_dist(0, p.num_locks - 1);
      std::uniform_int_distribution<int> op_dist(0, 99);
      std::uint64_t local_reads = 0, local_writes = 0;

      while (!stop.load(std::memory_order_relaxed)) {
        std::size_t idx = idx_dist(rng);
        if (op_dist(rng) < p.read_pct) {
          do {
            auto v = locks[idx].lock.read_begin();
            if (p.cs_work) busy_work(p.cs_work);
            if (locks[idx].lock.read_validate(v)) break;
          } while (true);
          if (measuring.load(std::memory_order_relaxed)) ++local_reads;
        } else {
          locks[idx].lock.write_lock();
          if (p.cs_work) busy_work(p.cs_work);
          locks[idx].lock.write_unlock();
          if (measuring.load(std::memory_order_relaxed)) ++local_writes;
        }
      }

      stats[t].reads  = local_reads;
      stats[t].writes = local_writes;
    });
  }

  barrier.wait_all_arrived();
  barrier.release();

  if (p.warmup_seconds > 0)
    std::this_thread::sleep_for(std::chrono::seconds(p.warmup_seconds));

  measuring.store(true, std::memory_order_relaxed);
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

  print_result(label, p, secs, total_reads + total_writes,
               total_reads, total_writes, per_thread);
}

int main(int argc, char** argv) {
  params p = parse_args(argc, argv);

  if      (p.lock_name == "tas")    bench_lock_array<tas_lock>(p, "tas");
  else if (p.lock_name == "ttas")   bench_lock_array<ttas_lock>(p, "ttas");
  else if (p.lock_name == "cas")    bench_lock_array<cas_lock>(p, "cas");
  else if (p.lock_name == "ticket") bench_lock_array<ticket_lock>(p, "ticket");
  else if (p.lock_name == "rw")     bench_lock_array_rw(p, "rw");
  else if (p.lock_name == "occ")    bench_lock_array_occ(p, "occ");
  else {
    std::cerr << "Unsupported --lock " << p.lock_name
              << " (use tas|ttas|cas|ticket|rw|occ)\n";
    return 2;
  }

  return 0;
}
