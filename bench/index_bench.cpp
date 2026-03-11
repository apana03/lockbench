// indexbench - test locks on a concurrent hash table
// supports uniform and zipfian key distributions

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
#include "../include/indexes/hash_index.hpp"
#include "../include/util/zipfian.hpp"

struct params {
  std::string lock_name  = "ttas";
  std::string dist       = "uniform";  // uniform | zipfian
  int    threads         = std::max(1u, std::thread::hardware_concurrency());
  int    seconds         = 5;
  int    warmup_seconds  = 2;
  int    read_pct        = 80;         // % lookups
  int    insert_pct      = 10;         // % inserts (rest = deletes)
  double zipf_theta      = 0.99;       // skew for Zipfian
  std::size_t num_buckets = 1 << 16;   // 65536 buckets
  std::uint64_t key_range = 1'000'000; // key space [0, key_range)
  std::size_t prefill     = 500'000;   // pre-populate before benchmark
  std::string csv_file;
};

static void usage() {
  std::cout <<
    "Usage: indexbench [OPTIONS]\n"
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
    "  --buckets <N>       Hash table buckets (power of 2) [default: 65536]\n"
    "  --key_range <N>     Key space size [default: 1000000]\n"
    "  --prefill <N>       Keys to pre-insert [default: 500000]\n"
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
    else if (a == "--help" || a == "-h") { usage(); std::exit(0); }
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

static void print_result(const char* label, const params& p, double secs,
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
    std::string header = "lock,dist,threads,buckets,key_range,read_pct,insert_pct,delete_pct,seconds,total_ops,gets,puts,removes,ops_s,ns_op,fairness_min,fairness_max,fairness_ratio";
    std::ostringstream row;
    row << label << "," << p.dist << "," << p.threads << ","
        << p.num_buckets << "," << p.key_range << "," << p.read_pct << ","
        << p.insert_pct << "," << delete_pct << "," << secs << ","
        << total << "," << total_gets << "," << total_puts << ","
        << total_rems << "," << static_cast<std::uint64_t>(ops_s) << ","
        << ns_op << "," << mn << "," << mx << "," << fairness;
    csv_append(p.csv_file, header, row.str());
  }
}

// shared benchmark driver: prefill, run workers, measure, print
// get_fn(index, key) is called for reads, put_fn(index, key) for inserts
template <class Index>
static void run_bench_common(const params& p, const char* label, Index& index,
                              auto get_fn, auto put_fn, auto remove_fn) {
  std::atomic<bool> stop{false};
  std::atomic<bool> measuring{false};
  start_barrier barrier(p.threads);

  std::vector<thread_stats> stats(p.threads);
  std::vector<std::thread> workers;
  workers.reserve(p.threads);

  for (int t = 0; t < p.threads; ++t) {
    workers.emplace_back([&, t] {
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

  print_result(label, p, secs, total_gets, total_puts, total_rems, per_thread);
}

template <class Lock>
static void run_index_bench(const params& p, const char* label) {
  hash_index<Lock> index(p.num_buckets);

  // fill in some data first so reads/deletes actually find stuff
  {
    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<std::uint64_t> key_dist(0, p.key_range - 1);
    for (std::size_t i = 0; i < p.prefill; ++i) {
      std::uint64_t k = key_dist(rng);
      index.put(k, k + 1);
    }
  }

  run_bench_common(p, label, index,
    [](auto& idx, std::uint64_t key) { idx.get(key); },
    [](auto& idx, std::uint64_t key) { idx.put(key, key + 1); },
    [](auto& idx, std::uint64_t key) { idx.remove(key); });
}

// rw lock version - reads use shared lock instead of exclusive
static void run_rw_index_bench(const params& p, const char* label) {
  hash_index<rw_lock> index(p.num_buckets);

  {
    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<std::uint64_t> key_dist(0, p.key_range - 1);
    for (std::size_t i = 0; i < p.prefill; ++i) {
      std::uint64_t k = key_dist(rng);
      index.put(k, k + 1);
    }
  }

  run_bench_common(p, label, index,
    [](auto& idx, std::uint64_t key) { idx.get_shared(key); },
    [](auto& idx, std::uint64_t key) { idx.put(key, key + 1); },
    [](auto& idx, std::uint64_t key) { idx.remove(key); });
}

// OCC version - reads don't take a lock at all, just validate after
static void run_occ_index_bench(const params& p, const char* label) {
  hash_index<occ_lock> index(p.num_buckets);

  {
    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<std::uint64_t> key_dist(0, p.key_range - 1);
    for (std::size_t i = 0; i < p.prefill; ++i) {
      std::uint64_t k = key_dist(rng);
      index.put(k, k + 1);
    }
  }

  run_bench_common(p, label, index,
    [](auto& idx, std::uint64_t key) { idx.get_optimistic(key); },
    [](auto& idx, std::uint64_t key) { idx.put(key, key + 1); },
    [](auto& idx, std::uint64_t key) { idx.remove(key); });
}

int main(int argc, char** argv) {
  params p = parse_args(argc, argv);
  if (p.lock_name == "rw") {
    run_rw_index_bench(p, "rw");
  } else if (p.lock_name == "occ") {
    run_occ_index_bench(p, "occ");
  } else if (p.lock_name == "tas") {
    run_index_bench<tas_lock>(p, "tas");
  } else if (p.lock_name == "ttas") {
    run_index_bench<ttas_lock>(p, "ttas");
  } else if (p.lock_name == "cas") {
    run_index_bench<cas_lock>(p, "cas");
  } else if (p.lock_name == "ticket") {
    run_index_bench<ticket_lock>(p, "ticket");
  } else {
    std::cerr << "Unsupported --lock " << p.lock_name
              << " (use tas|ttas|cas|ticket|rw|occ)\n";
    return 2;
  }

  return 0;
}
