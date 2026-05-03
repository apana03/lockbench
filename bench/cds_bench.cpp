// cdsbench - libcds StripedMap with pluggable per-stripe lock
// Mirrors indexbench's CLI; --lock selects the stripe primitive.

#include <iostream>
#include <mutex>
#include <string>

#include "../include/primitives/tas_lock.hpp"
#include "../include/primitives/ttas_lock.hpp"
#include "../include/primitives/cas_lock.hpp"
#include "../include/primitives/ticket_lock.hpp"
#include "../include/indexes/striped_map_index.hpp"
#include "../include/util/bench_harness.hpp"

template <class Lock>
static void run_cds_bench(const params& p, const char* label) {
  striped_map_index<Lock> index(p.num_buckets);
  prefill_index(index, p);

  run_bench_common(p, label, index,
    [](auto& idx, std::uint64_t key) { idx.get(key); },
    [](auto& idx, std::uint64_t key) { idx.put(key, key + 1); },
    [](auto& idx, std::uint64_t key) { idx.remove(key); });
}

int main(int argc, char** argv) {
  params p = parse_bench_args(argc, argv);
  if (p.lock_name == "std")         run_cds_bench<std::mutex>(p, "cds-std");
  else if (p.lock_name == "tas")    run_cds_bench<tas_lock>(p, "cds-tas");
  else if (p.lock_name == "ttas")   run_cds_bench<ttas_lock>(p, "cds-ttas");
  else if (p.lock_name == "cas")    run_cds_bench<cas_lock>(p, "cds-cas");
  else if (p.lock_name == "ticket") run_cds_bench<ticket_lock>(p, "cds-ticket");
  else {
    std::cerr << "Unsupported --lock " << p.lock_name
              << " (use std|tas|ttas|cas|ticket)\n";
    return 2;
  }
  return 0;
}
