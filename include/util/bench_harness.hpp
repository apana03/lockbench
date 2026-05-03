#pragma once
// Shared benchmark harness for index-style workloads: prefill + workers + CSV.
// Consumed by index_bench, skiplist_bench, bptree_bench. Each caller supplies
// its own Index type and lookup/insert/remove lambdas.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../primitives/util.hpp"
#include "zipfian.hpp"

// Returns a short architecture tag: $LB_ARCH if set, else uname -m.
inline std::string detect_arch() {
  if (const char* e = std::getenv("LB_ARCH"); e && *e) return std::string(e);
  struct utsname u{};
  if (uname(&u) == 0) return std::string(u.machine);
  return std::string("unknown");
}

// Hostname for cross-arch result tagging. Truncated to 63 chars; no domain.
inline std::string detect_hostname() {
  char buf[64] = {0};
  if (gethostname(buf, sizeof(buf) - 1) != 0) return std::string("unknown");
  if (char* dot = std::strchr(buf, '.')) *dot = '\0';
  return std::string(buf);
}

struct params {
  std::string lock_name  = "ttas";
  std::string dist       = "uniform";
  int    threads         = std::max(1u, std::thread::hardware_concurrency());
  int    seconds         = 5;
  int    warmup_seconds  = 2;
  int    read_pct        = 80;
  int    insert_pct      = 10;
  double zipf_theta      = 0.99;
  std::size_t num_buckets = 1 << 16;      // hash only; ignored elsewhere
  std::uint64_t key_range = 1'000'000;
  std::size_t prefill     = 500'000;
  std::string csv_file;
  bool pin              = false;
};

inline void bench_usage(const char* prog) {
  std::cout <<
    "Usage: " << prog << " [OPTIONS]\n"
    "\n"
    "Options:\n"
    "  --lock <name>       Lock primitive (tas|ttas|cas|ticket|rw|occ)\n"
    "  --dist <d>          Key distribution (uniform|zipfian) [default: uniform]\n"
    "  --threads <N>       Worker threads [default: hw_concurrency]\n"
    "  --seconds <S>       Measurement seconds [default: 5]\n"
    "  --warmup <S>        Warmup seconds [default: 2]\n"
    "  --read_pct <P>      Lookup percentage (0-100) [default: 80]\n"
    "  --insert_pct <P>    Insert percentage (0-100) [default: 10]\n"
    "  --zipf_theta <T>    Zipfian skew (0.0-0.99) [default: 0.99]\n"
    "  --buckets <N>       Hash table buckets (hash only) [default: 65536]\n"
    "  --key_range <N>     Key space size [default: 1000000]\n"
    "  --prefill <N>       Keys to pre-insert [default: 500000]\n"
    "  --pin               Pin threads to cores (Linux only)\n"
    "  --csv <file>        Append results as CSV to file\n";
}

inline params parse_bench_args(int argc, char** argv) {
  params p;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) { std::cerr << "Missing value for " << name << "\n"; std::exit(2); }
      return std::string(argv[++i]);
    };
    if      (a == "--lock")       p.lock_name     = need("--lock");
    else if (a == "--dist")       p.dist          = need("--dist");
    else if (a == "--threads")    p.threads       = std::stoi(need("--threads"));
    else if (a == "--seconds")    p.seconds       = std::stoi(need("--seconds"));
    else if (a == "--warmup")     p.warmup_seconds = std::stoi(need("--warmup"));
    else if (a == "--read_pct")   p.read_pct      = std::stoi(need("--read_pct"));
    else if (a == "--insert_pct") p.insert_pct    = std::stoi(need("--insert_pct"));
    else if (a == "--zipf_theta") p.zipf_theta    = std::stod(need("--zipf_theta"));
    else if (a == "--buckets")    p.num_buckets   = std::stoull(need("--buckets"));
    else if (a == "--key_range")  p.key_range     = std::stoull(need("--key_range"));
    else if (a == "--prefill")    p.prefill       = std::stoull(need("--prefill"));
    else if (a == "--csv")        p.csv_file      = need("--csv");
    else if (a == "--pin")        p.pin           = true;
    else if (a == "--help" || a == "-h") { bench_usage(argv[0]); std::exit(0); }
    else { std::cerr << "Unknown arg: " << a << "\n"; std::exit(2); }
  }
  p.threads = std::max(p.threads, 1);
  p.seconds = std::max(p.seconds, 1);
  p.warmup_seconds = std::max(p.warmup_seconds, 0);
  p.read_pct = std::clamp(p.read_pct, 0, 100);
  p.insert_pct = std::clamp(p.insert_pct, 0, 100 - p.read_pct);
  return p;
}

struct alignas(64) thread_stats {
  std::uint64_t gets    = 0;
  std::uint64_t puts    = 0;
  std::uint64_t removes = 0;
};

inline void csv_append(const std::string& path, const std::string& header,
                       const std::string& row) {
  bool write_header = false;
  struct stat st;
  if (stat(path.c_str(), &st) != 0 || st.st_size == 0)
    write_header = true;
  std::ofstream f(path, std::ios::app);
  if (write_header) f << header << "\n";
  f << row << "\n";
}

inline void print_bench_result(const char* label, const params& p, double secs,
                               std::uint64_t total_gets, std::uint64_t total_puts,
                               std::uint64_t total_rems,
                               const std::vector<std::uint64_t>& per_thread) {
  int delete_pct = 100 - p.read_pct - p.insert_pct;
  std::uint64_t total = total_gets + total_puts + total_rems;
  double ops_s = static_cast<double>(total) / secs;
  double ns_op = (secs * 1e9) / std::max<std::uint64_t>(1, total);

  std::cout
    << "lock=" << label
    << " dist=" << p.dist
    << " threads=" << p.threads
    << " buckets=" << p.num_buckets
    << " key_range=" << p.key_range
    << " read_pct=" << p.read_pct
    << " insert_pct=" << p.insert_pct
    << " delete_pct=" << delete_pct
    << " seconds=" << secs
    << " total_ops=" << total
    << " gets=" << total_gets
    << " puts=" << total_puts
    << " removes=" << total_rems
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
    // Cache arch+hostname (cheap, but no need to re-query per row).
    static const std::string arch = detect_arch();
    static const std::string host = detect_hostname();
    std::string header = "arch;hostname;lock;dist;zipf_theta;threads;buckets;key_range;read_pct;insert_pct;delete_pct;total_ops;gets;puts;removes;ops_s;ns_op;fairness_min;fairness_max;fairness_ratio";
    std::ostringstream row;
    row << arch << ";" << host << ";"
        << label << ";" << p.dist << ";" << fmt_double(p.zipf_theta) << ";"
        << p.threads << ";"
        << p.num_buckets << ";" << p.key_range << ";" << p.read_pct << ";"
        << p.insert_pct << ";" << delete_pct << ";"
        << total << ";" << total_gets << ";" << total_puts << ";"
        << total_rems << ";" << static_cast<std::uint64_t>(ops_s) << ";"
        << fmt_double(ns_op) << ";" << mn << ";" << mx << ";" << fmt_double(fairness);
    csv_append(p.csv_file, header, row.str());
  }
}

// Generic driver: spawns workers, prefill must already be done, each thread runs
// get_fn / put_fn / remove_fn mixed according to read_pct / insert_pct.
template <class Index, class GetFn, class PutFn, class RemFn>
inline void run_bench_common(const params& p, const char* label, Index& index,
                             GetFn get_fn, PutFn put_fn, RemFn remove_fn) {
  std::atomic<bool> stop{false};
  std::atomic<bool> measuring{false};
  start_barrier barrier(p.threads);

  std::vector<thread_stats> stats(p.threads);
  std::vector<std::thread> workers;
  workers.reserve(p.threads);

  for (int t = 0; t < p.threads; ++t) {
    workers.emplace_back([&, t] {
      setup_worker_thread(t, p.pin);
      barrier.arrive_and_wait();

      std::uint64_t local_gets = 0, local_puts = 0, local_rems = 0;

      if (p.dist == "zipfian") {
        zipfian_generator gen(p.key_range, p.zipf_theta, t + 100);
        std::mt19937 op_rng(t + 200);
        std::uniform_int_distribution<int> op_dist(0, 99);

        while (!stop.load(std::memory_order_relaxed)) {
          std::uint64_t key = gen.next_scrambled();
          int op = op_dist(op_rng);
          if (op < p.read_pct) {
            get_fn(index, key);
            if (measuring.load(std::memory_order_relaxed)) ++local_gets;
          } else if (op < p.read_pct + p.insert_pct) {
            put_fn(index, key);
            if (measuring.load(std::memory_order_relaxed)) ++local_puts;
          } else {
            remove_fn(index, key);
            if (measuring.load(std::memory_order_relaxed)) ++local_rems;
          }
        }
      } else {
        std::mt19937_64 key_rng(t + 100);
        std::uniform_int_distribution<std::uint64_t> key_dist(0, p.key_range - 1);
        std::mt19937 op_rng(t + 200);
        std::uniform_int_distribution<int> op_dist(0, 99);

        while (!stop.load(std::memory_order_relaxed)) {
          std::uint64_t key = key_dist(key_rng);
          int op = op_dist(op_rng);
          if (op < p.read_pct) {
            get_fn(index, key);
            if (measuring.load(std::memory_order_relaxed)) ++local_gets;
          } else if (op < p.read_pct + p.insert_pct) {
            put_fn(index, key);
            if (measuring.load(std::memory_order_relaxed)) ++local_puts;
          } else {
            remove_fn(index, key);
            if (measuring.load(std::memory_order_relaxed)) ++local_rems;
          }
        }
      }

      stats[t].gets    = local_gets;
      stats[t].puts    = local_puts;
      stats[t].removes = local_rems;
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

  std::uint64_t total_gets = 0, total_puts = 0, total_rems = 0;
  std::vector<std::uint64_t> per_thread(p.threads);
  for (int t = 0; t < p.threads; ++t) {
    total_gets += stats[t].gets;
    total_puts += stats[t].puts;
    total_rems += stats[t].removes;
    per_thread[t] = stats[t].gets + stats[t].puts + stats[t].removes;
  }

  print_bench_result(label, p, secs, total_gets, total_puts, total_rems, per_thread);
}

// Uniformly prefill keys into an index with put(key, key+1).
template <class Index>
inline void prefill_index(Index& index, const params& p) {
  std::mt19937_64 rng(12345);
  std::uniform_int_distribution<std::uint64_t> key_dist(0, p.key_range - 1);
  for (std::size_t i = 0; i < p.prefill; ++i) {
    std::uint64_t k = key_dist(rng);
    index.put(k, k + 1);
  }
}
