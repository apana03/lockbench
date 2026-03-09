// arraybench - test locks on a shared array
// "single" mode = one lock for the whole thing, "striped" = one lock per partition
// reads scan a few elements, writes update one element

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numeric>
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

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------
struct params {
  std::string lock_name  = "ttas";
  std::string mode       = "single";   // single | striped
  int    threads         = std::max(1u, std::thread::hardware_concurrency());
  int    seconds         = 3;
  int    warmup_seconds  = 1;
  int    read_pct        = 80;         // % reads
  std::size_t array_size = 1 << 16;    // 65536 elements
  std::size_t num_stripes = 64;        // stripes for striped mode
  int    scan_len        = 16;         // elements read per scan
};

static void usage() {
  std::cout <<
    "Usage: arraybench [OPTIONS]\n"
    "\n"
    "Options:\n"
    "  --lock <name>       Lock primitive (tas|ttas|cas|ticket|rw|occ)\n"
    "  --mode <m>          Locking mode (single|striped) [default: single]\n"
    "  --threads <N>       Worker threads [default: hw_concurrency]\n"
    "  --seconds <S>       Measurement seconds [default: 3]\n"
    "  --warmup <S>        Warmup seconds [default: 1]\n"
    "  --read_pct <P>      Read percentage (0-100) [default: 80]\n"
    "  --array_size <N>    Array size [default: 65536]\n"
    "  --stripes <N>       Number of stripes for striped mode [default: 64]\n"
    "  --scan_len <N>      Elements per read scan [default: 16]\n";
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
    else if (a == "--mode")       p.mode          = need("--mode");
    else if (a == "--threads")    p.threads       = std::stoi(need("--threads"));
    else if (a == "--seconds")    p.seconds       = std::stoi(need("--seconds"));
    else if (a == "--warmup")     p.warmup_seconds = std::stoi(need("--warmup"));
    else if (a == "--read_pct")   p.read_pct      = std::stoi(need("--read_pct"));
    else if (a == "--array_size") p.array_size    = std::stoull(need("--array_size"));
    else if (a == "--stripes")    p.num_stripes   = std::stoull(need("--stripes"));
    else if (a == "--scan_len")   p.scan_len      = std::stoi(need("--scan_len"));
    else if (a == "--help" || a == "-h") { usage(); std::exit(0); }
    else { std::cerr << "Unknown arg: " << a << "\n"; std::exit(2); }
  }
  p.threads = std::max(p.threads, 1);
  p.seconds = std::max(p.seconds, 1);
  p.warmup_seconds = std::max(p.warmup_seconds, 0);
  p.read_pct = std::clamp(p.read_pct, 0, 100);
  p.scan_len = std::max(p.scan_len, 1);
  return p;
}

struct alignas(64) thread_stats {
  std::uint64_t reads  = 0;
  std::uint64_t writes = 0;
};

static void print_result(const char* lock_label, const char* mode_label,
                          const params& p, double secs,
                          std::uint64_t total_reads, std::uint64_t total_writes,
                          const std::vector<std::uint64_t>& per_thread) {
  std::uint64_t total = total_reads + total_writes;
  double ops_s = static_cast<double>(total) / secs;
  double ns_op = (secs * 1e9) / std::max<std::uint64_t>(1, total);

  std::cout
    << "lock=" << lock_label
    << " mode=" << mode_label
    << " threads=" << p.threads
    << " array_size=" << p.array_size
    << " stripes=" << (p.mode == "striped" ? p.num_stripes : 1)
    << " scan_len=" << p.scan_len
    << " read_pct=" << p.read_pct
    << " seconds=" << secs
    << " total_ops=" << total
    << " reads=" << total_reads
    << " writes=" << total_writes
    << " ops_s=" << static_cast<std::uint64_t>(ops_s)
    << " ns_op=" << ns_op
    << "\n";

  if (p.threads > 1) {
    std::uint64_t mn = *std::min_element(per_thread.begin(), per_thread.end());
    std::uint64_t mx = *std::max_element(per_thread.begin(), per_thread.end());
    double fairness = (mx > 0) ? static_cast<double>(mn) / static_cast<double>(mx) : 1.0;
    std::cout
      << "  fairness: min=" << mn << " max=" << mx
      << " ratio=" << fairness << "\n";
  }
}

template <class Lock>
static void bench_single(const params& p, const char* label) {
  std::vector<std::uint64_t> arr(p.array_size, 1);
  Lock lock;

  std::atomic<bool> stop{false};
  start_barrier barrier(p.threads);

  std::vector<thread_stats> stats(p.threads);
  std::vector<std::thread> workers;
  workers.reserve(p.threads);

  for (int t = 0; t < p.threads; ++t) {
    workers.emplace_back([&, t] {
      barrier.arrive_and_wait();
      std::mt19937_64 rng(t + 42);
      std::uniform_int_distribution<std::size_t> idx_dist(0, p.array_size - 1);
      std::uniform_int_distribution<int> op_dist(0, 99);
      std::uint64_t local_reads = 0, local_writes = 0;

      while (!stop.load(std::memory_order_relaxed)) {
        std::size_t idx = idx_dist(rng);
        if (op_dist(rng) < p.read_pct) {
          lock.lock();
          std::uint64_t sum = 0;
          for (int s = 0; s < p.scan_len; ++s) {
            sum += arr[(idx + s) % p.array_size];
          }
          lock.unlock();
          volatile std::uint64_t sink = sum; (void)sink;
          ++local_reads;
        } else {
          lock.lock();
          arr[idx] = arr[idx] + 1;
          lock.unlock();
          ++local_writes;
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

  print_result(label, "single", p, secs, total_reads, total_writes, per_thread);
}

// rw version - readers can go in parallel
static void bench_single_rw(const params& p, const char* label) {
  std::vector<std::uint64_t> arr(p.array_size, 1);
  rw_lock lock;

  std::atomic<bool> stop{false};
  start_barrier barrier(p.threads);

  std::vector<thread_stats> stats(p.threads);
  std::vector<std::thread> workers;
  workers.reserve(p.threads);

  for (int t = 0; t < p.threads; ++t) {
    workers.emplace_back([&, t] {
      barrier.arrive_and_wait();
      std::mt19937_64 rng(t + 42);
      std::uniform_int_distribution<std::size_t> idx_dist(0, p.array_size - 1);
      std::uniform_int_distribution<int> op_dist(0, 99);
      std::uint64_t local_reads = 0, local_writes = 0;

      while (!stop.load(std::memory_order_relaxed)) {
        std::size_t idx = idx_dist(rng);
        if (op_dist(rng) < p.read_pct) {
          lock.read_lock();
          std::uint64_t sum = 0;
          for (int s = 0; s < p.scan_len; ++s) {
            sum += arr[(idx + s) % p.array_size];
          }
          lock.read_unlock();
          volatile std::uint64_t sink = sum; (void)sink;
          ++local_reads;
        } else {
          lock.write_lock();
          arr[idx] = arr[idx] + 1;
          lock.write_unlock();
          ++local_writes;
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

  print_result(label, "single", p, secs, total_reads, total_writes, per_thread);
}

// occ version - readers just read and check version after
static void bench_single_occ(const params& p, const char* label) {
  std::vector<std::uint64_t> arr(p.array_size, 1);
  occ_lock lock;

  std::atomic<bool> stop{false};
  start_barrier barrier(p.threads);

  std::vector<thread_stats> stats(p.threads);
  std::vector<std::thread> workers;
  workers.reserve(p.threads);

  for (int t = 0; t < p.threads; ++t) {
    workers.emplace_back([&, t] {
      barrier.arrive_and_wait();
      std::mt19937_64 rng(t + 42);
      std::uniform_int_distribution<std::size_t> idx_dist(0, p.array_size - 1);
      std::uniform_int_distribution<int> op_dist(0, 99);
      std::uint64_t local_reads = 0, local_writes = 0;

      while (!stop.load(std::memory_order_relaxed)) {
        std::size_t idx = idx_dist(rng);
        if (op_dist(rng) < p.read_pct) {
          std::uint64_t sum;
          do {
            auto v = lock.read_begin();
            sum = 0;
            for (int s = 0; s < p.scan_len; ++s) {
              sum += arr[(idx + s) % p.array_size];
            }
            if (lock.read_validate(v)) break;
          } while (true);
          volatile std::uint64_t sink = sum; (void)sink;
          ++local_reads;
        } else {
          lock.write_lock();
          arr[idx] = arr[idx] + 1;
          lock.write_unlock();
          ++local_writes;
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

  print_result(label, "single", p, secs, total_reads, total_writes, per_thread);
}

// striped version - split the array into chunks, each with its own lock
template <class Lock>
static void bench_striped(const params& p, const char* label) {
  std::vector<std::uint64_t> arr(p.array_size, 1);
  std::size_t stripe_size = (p.array_size + p.num_stripes - 1) / p.num_stripes;

  struct alignas(64) stripe_lock { Lock lock; };
  std::vector<stripe_lock> locks(p.num_stripes);

  std::atomic<bool> stop{false};
  start_barrier barrier(p.threads);

  std::vector<thread_stats> stats(p.threads);
  std::vector<std::thread> workers;
  workers.reserve(p.threads);

  for (int t = 0; t < p.threads; ++t) {
    workers.emplace_back([&, t] {
      barrier.arrive_and_wait();
      std::mt19937_64 rng(t + 42);
      std::uniform_int_distribution<std::size_t> idx_dist(0, p.array_size - 1);
      std::uniform_int_distribution<int> op_dist(0, 99);
      std::uint64_t local_reads = 0, local_writes = 0;

      while (!stop.load(std::memory_order_relaxed)) {
        std::size_t idx = idx_dist(rng);
        std::size_t stripe = idx / stripe_size;
        if (stripe >= p.num_stripes) stripe = p.num_stripes - 1;

        if (op_dist(rng) < p.read_pct) {
          locks[stripe].lock.lock();
          std::uint64_t sum = 0;
          std::size_t start = stripe * stripe_size;
          std::size_t end = std::min(start + stripe_size, p.array_size);
          std::size_t scan = std::min(static_cast<std::size_t>(p.scan_len), end - start);
          std::size_t read_idx = start + (idx - start) % (end - start);
          for (std::size_t s = 0; s < scan; ++s) {
            std::size_t ri = start + (read_idx - start + s) % (end - start);
            sum += arr[ri];
          }
          locks[stripe].lock.unlock();
          volatile std::uint64_t sink = sum; (void)sink;
          ++local_reads;
        } else {
          locks[stripe].lock.lock();
          arr[idx] = arr[idx] + 1;
          locks[stripe].lock.unlock();
          ++local_writes;
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

  print_result(label, "striped", p, secs, total_reads, total_writes, per_thread);
}

int main(int argc, char** argv) {
  params p = parse_args(argc, argv);

  if (p.mode == "single") {
    if      (p.lock_name == "tas")    bench_single<tas_lock>(p, "tas");
    else if (p.lock_name == "ttas")   bench_single<ttas_lock>(p, "ttas");
    else if (p.lock_name == "cas")    bench_single<cas_lock>(p, "cas");
    else if (p.lock_name == "ticket") bench_single<ticket_lock>(p, "ticket");
    else if (p.lock_name == "rw")     bench_single_rw(p, "rw");
    else if (p.lock_name == "occ")    bench_single_occ(p, "occ");
    else {
      std::cerr << "Unsupported --lock " << p.lock_name << "\n";
      return 2;
    }
  } else if (p.mode == "striped") {
    if      (p.lock_name == "tas")    bench_striped<tas_lock>(p, "tas");
    else if (p.lock_name == "ttas")   bench_striped<ttas_lock>(p, "ttas");
    else if (p.lock_name == "cas")    bench_striped<cas_lock>(p, "cas");
    else if (p.lock_name == "ticket") bench_striped<ticket_lock>(p, "ticket");
    else if (p.lock_name == "rw")     bench_striped<rw_lock>(p, "rw");
    else if (p.lock_name == "occ")    bench_striped<occ_lock>(p, "occ");
    else {
      std::cerr << "Unsupported --lock " << p.lock_name << "\n";
      return 2;
    }
  } else {
    std::cerr << "Unknown --mode " << p.mode << " (use single|striped)\n";
    return 2;
  }

  return 0;
}
