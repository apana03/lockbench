// indextest - correctness probe for skiplist_index and bptree_index
//
// --mode single : single-threaded oracle test against std::map. Exhaustively
//                 interleaves put/get/remove and verifies every lookup matches
//                 the oracle. Also runs sized probes that straddle B+ tree
//                 fanout boundaries to exercise splits and root replacement.
//
// --mode race   : N threads run random put/remove/get on disjoint key slices.
//                 Each thread owns its slice, so there are no lost updates to
//                 detect. After the run, we walk the structure single-threaded
//                 and assert that every key a thread believes it "put" (and
//                 didn't remove) is still present.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../include/primitives/tas_lock.hpp"
#include "../include/primitives/ttas_lock.hpp"
#include "../include/primitives/cas_lock.hpp"
#include "../include/primitives/ticket_lock.hpp"
#include "../include/primitives/rw_lock.hpp"
#include "../include/primitives/occ.hpp"
#include "../include/indexes/skiplist_index.hpp"
#include "../include/indexes/bptree_index.hpp"

static int failures = 0;

#define CHECK(cond, msg) \
  do { if (!(cond)) { std::cerr << "FAIL: " << (msg) << " at " << __FILE__ \
                                << ":" << __LINE__ << "\n"; ++failures; } } while (0)

template <class Index>
static void single_threaded_oracle(Index& idx, std::uint64_t key_range,
                                   std::uint64_t ops, std::uint32_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<std::uint64_t> key_dist(0, key_range - 1);
  std::uniform_int_distribution<int> op_dist(0, 99);
  std::map<std::uint64_t, std::uint64_t> oracle;

  for (std::uint64_t i = 0; i < ops; ++i) {
    std::uint64_t k = key_dist(rng);
    int op = op_dist(rng);
    if (op < 40) {
      auto got = idx.get(k);
      auto it = oracle.find(k);
      if (it == oracle.end()) {
        CHECK(!got.has_value(), "get returned value for missing key");
      } else {
        CHECK(got.has_value() && *got == it->second, "get mismatch with oracle");
      }
    } else if (op < 75) {
      std::uint64_t v = k * 2 + 1;
      idx.put(k, v);
      oracle[k] = v;
    } else {
      idx.remove(k);
      oracle.erase(k);
    }
  }

  // Final sweep: every oracle key must be present with matching value.
  for (auto& [k, v] : oracle) {
    auto got = idx.get(k);
    CHECK(got.has_value() && *got == v, "final sweep: oracle key missing");
  }
}

template <class Index>
static void race_test(Index& idx, int threads, std::uint64_t per_thread_keys,
                      std::uint64_t ops_per_thread) {
  // Each thread owns a disjoint slice [t * per_thread_keys, (t+1)*per_thread_keys).
  // We track "currently present" membership per slice. Final pass: for every key
  // the thread last put and didn't subsequently remove, assert the index returns it.
  std::vector<std::unordered_map<std::uint64_t, std::uint64_t>> owned(threads);
  std::vector<std::thread> workers;
  std::atomic<bool> go{false};
  std::vector<std::unordered_map<std::uint64_t, std::uint64_t>> final_state(threads);

  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t]() {
      while (!go.load(std::memory_order_acquire)) cpu_relax();
      std::mt19937_64 rng(0x9E37 + t);
      std::uniform_int_distribution<std::uint64_t> key_off(0, per_thread_keys - 1);
      std::uniform_int_distribution<int> op_dist(0, 99);
      std::uint64_t base = static_cast<std::uint64_t>(t) * per_thread_keys;
      std::unordered_map<std::uint64_t, std::uint64_t> mine;

      for (std::uint64_t i = 0; i < ops_per_thread; ++i) {
        std::uint64_t k = base + key_off(rng);
        int op = op_dist(rng);
        if (op < 40) {
          auto got = idx.get(k);
          auto it = mine.find(k);
          if (it != mine.end()) {
            // We own this key and believe it's present. But another thread
            // never touches it, so this must match.
            CHECK(got.has_value() && *got == it->second,
                  "race: owned-key lookup mismatch");
          }
        } else if (op < 75) {
          std::uint64_t v = (k << 1) | 1;
          idx.put(k, v);
          mine[k] = v;
        } else {
          idx.remove(k);
          mine.erase(k);
        }
      }
      final_state[t] = std::move(mine);
    });
  }
  go.store(true, std::memory_order_release);
  for (auto& th : workers) th.join();

  for (int t = 0; t < threads; ++t) {
    for (auto& [k, v] : final_state[t]) {
      auto got = idx.get(k);
      CHECK(got.has_value() && *got == v, "race: post-run owned key missing");
    }
  }
}

template <class Index>
static void bptree_split_probes(Index& idx) {
  // Populate sizes that straddle fanout boundaries to force splits / root changes.
  const std::uint64_t sizes[] = {1, 16, 17, 256, 257, 1000, 10000};
  for (std::uint64_t n : sizes) {
    Index local;
    for (std::uint64_t i = 0; i < n; ++i) local.put(i, i + 7);
    for (std::uint64_t i = 0; i < n; ++i) {
      auto got = local.get(i);
      CHECK(got.has_value() && *got == i + 7, "bptree split probe: missing key");
    }
  }
  (void)idx;
}

// --- dispatch ---------------------------------------------------------------

struct cfg {
  std::string structure = "skiplist";   // skiplist | bptree
  std::string lock      = "ttas";       // tas|ttas|cas|ticket|rw|occ
  std::string mode      = "single";     // single | race
  int threads           = 8;
  std::uint64_t key_range        = 1'000;
  std::uint64_t ops              = 50'000;
  std::uint64_t per_thread_keys  = 256;
  std::uint64_t ops_per_thread   = 20'000;
  std::uint32_t seed             = 42;
};

static cfg parse(int argc, char** argv) {
  cfg c;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) { std::cerr << "Missing value for " << name << "\n"; std::exit(2); }
      return std::string(argv[++i]);
    };
    if      (a == "--structure") c.structure = need("--structure");
    else if (a == "--lock")      c.lock      = need("--lock");
    else if (a == "--mode")      c.mode      = need("--mode");
    else if (a == "--threads")   c.threads   = std::stoi(need("--threads"));
    else if (a == "--key_range") c.key_range = std::stoull(need("--key_range"));
    else if (a == "--ops")       c.ops       = std::stoull(need("--ops"));
    else if (a == "--per_thread_keys") c.per_thread_keys = std::stoull(need("--per_thread_keys"));
    else if (a == "--ops_per_thread")  c.ops_per_thread  = std::stoull(need("--ops_per_thread"));
    else if (a == "--seed")      c.seed      = static_cast<std::uint32_t>(std::stoul(need("--seed")));
    else if (a == "--help" || a == "-h") {
      std::cout <<
        "Usage: indextest [OPTIONS]\n"
        "  --structure <skiplist|bptree>\n"
        "  --lock <tas|ttas|cas|ticket|rw|occ>\n"
        "  --mode <single|race>\n"
        "  --threads <N>\n"
        "  --key_range <N>     (single mode)\n"
        "  --ops <N>           (single mode)\n"
        "  --per_thread_keys <N>  (race mode)\n"
        "  --ops_per_thread <N>   (race mode)\n"
        "  --seed <N>\n";
      std::exit(0);
    } else {
      std::cerr << "Unknown arg: " << a << "\n"; std::exit(2);
    }
  }
  return c;
}

template <template <class> class Index, class Lock>
static void run_one(const cfg& c) {
  if (c.mode == "single") {
    Index<Lock> idx;
    single_threaded_oracle(idx, c.key_range, c.ops, c.seed);
    if constexpr (std::is_same_v<Index<Lock>, bptree_index<Lock>>) {
      bptree_split_probes(idx);
    }
  } else {
    Index<Lock> idx;
    race_test(idx, c.threads, c.per_thread_keys, c.ops_per_thread);
  }
}

template <class Lock>
static void dispatch_lock_skiplist(const cfg& c) { run_one<skiplist_index, Lock>(c); }

template <class Lock>
static void dispatch_lock_bptree(const cfg& c) { run_one<bptree_index, Lock>(c); }

int main(int argc, char** argv) {
  cfg c = parse(argc, argv);
  std::cout << "indextest structure=" << c.structure
            << " lock=" << c.lock
            << " mode=" << c.mode
            << " threads=" << c.threads << "\n";

  auto dispatch = [&](auto fn) {
    if      (c.lock == "tas")    fn.template operator()<tas_lock>(c);
    else if (c.lock == "ttas")   fn.template operator()<ttas_lock>(c);
    else if (c.lock == "cas")    fn.template operator()<cas_lock>(c);
    else if (c.lock == "ticket") fn.template operator()<ticket_lock>(c);
    else if (c.lock == "rw")     fn.template operator()<rw_lock>(c);
    else if (c.lock == "occ")    fn.template operator()<occ_lock>(c);
    else { std::cerr << "Unknown lock: " << c.lock << "\n"; std::exit(2); }
  };

  if (c.structure == "skiplist") {
    dispatch([]<class L>(const cfg& cc) { run_one<skiplist_index, L>(cc); });
  } else if (c.structure == "bptree") {
    dispatch([]<class L>(const cfg& cc) { run_one<bptree_index, L>(cc); });
  } else {
    std::cerr << "Unknown structure: " << c.structure << "\n";
    return 2;
  }

  if (failures == 0) std::cout << "  OK\n";
  else               std::cout << "  FAILED (" << failures << " errors)\n";
  return failures == 0 ? 0 : 1;
}
