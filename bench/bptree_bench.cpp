// bptreebench - test locks inside a per-node-locked B+ tree (crabbing).
// supports uniform and zipfian key distributions

#include <iostream>
#include <string>

#include "../include/primitives/tas_lock.hpp"
#include "../include/primitives/ttas_lock.hpp"
#include "../include/primitives/cas_lock.hpp"
#include "../include/primitives/ticket_lock.hpp"
#include "../include/primitives/rw_lock.hpp"
#include "../include/primitives/occ.hpp"
#include "../include/indexes/bptree_index.hpp"
#include "../include/util/bench_harness.hpp"

template <class Lock>
static void run_bptree_bench(const params& p, const char* label) {
  bptree_index<Lock> index;
  prefill_index(index, p);

  run_bench_common(p, label, index,
    [](auto& idx, std::uint64_t key) { idx.get(key); },
    [](auto& idx, std::uint64_t key) { idx.put(key, key + 1); },
    [](auto& idx, std::uint64_t key) { idx.remove(key); });
}

static void run_rw_bptree_bench(const params& p, const char* label) {
  bptree_index<rw_lock> index;
  prefill_index(index, p);

  run_bench_common(p, label, index,
    [](auto& idx, std::uint64_t key) { idx.get_shared(key); },
    [](auto& idx, std::uint64_t key) { idx.put(key, key + 1); },
    [](auto& idx, std::uint64_t key) { idx.remove(key); });
}

static void run_occ_bptree_bench(const params& p, const char* label) {
  bptree_index<occ_lock> index;
  prefill_index(index, p);

  run_bench_common(p, label, index,
    [](auto& idx, std::uint64_t key) { idx.get_optimistic(key); },
    [](auto& idx, std::uint64_t key) { idx.put(key, key + 1); },
    [](auto& idx, std::uint64_t key) { idx.remove(key); });
}

int main(int argc, char** argv) {
  params p = parse_bench_args(argc, argv);
  if (p.lock_name == "rw") {
    run_rw_bptree_bench(p, "rw");
  } else if (p.lock_name == "occ") {
    run_occ_bptree_bench(p, "occ");
  } else if (p.lock_name == "tas") {
    run_bptree_bench<tas_lock>(p, "tas");
  } else if (p.lock_name == "ttas") {
    run_bptree_bench<ttas_lock>(p, "ttas");
  } else if (p.lock_name == "cas") {
    run_bptree_bench<cas_lock>(p, "cas");
  } else if (p.lock_name == "ticket") {
    run_bptree_bench<ticket_lock>(p, "ticket");
  } else {
    std::cerr << "Unsupported --lock " << p.lock_name
              << " (use tas|ttas|cas|ticket|rw|occ)\n";
    return 2;
  }

  return 0;
}
