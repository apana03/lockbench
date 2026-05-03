// cds_avl_bench - libcds BronsonAVLTreeMap with pluggable per-node lock.
// Mirrors cdsbench's CLI; --lock selects the per-node lock primitive
// injected via cds::sync::injecting_monitor<Lock>.

#include <iostream>
#include <mutex>
#include <string>

#include "../include/primitives/tas_lock.hpp"
#include "../include/primitives/ttas_lock.hpp"
#include "../include/primitives/cas_lock.hpp"
#include "../include/primitives/ticket_lock.hpp"
#include "../include/indexes/avl_tree_index.hpp"
#include "../include/util/bench_harness.hpp"

template <class Lock>
static void run_avl_bench(const params& p, const char* label) {
    avl_tree_index<Lock> index;
    prefill_index(index, p);

    run_bench_common(p, label, index,
        [](auto& idx, std::uint64_t key) { idx.get(key); },
        [](auto& idx, std::uint64_t key) { idx.put(key, key + 1); },
        [](auto& idx, std::uint64_t key) { idx.remove(key); });
}

int main(int argc, char** argv) {
    params p = parse_bench_args(argc, argv);

    cds::Initialize();
    int rc = 0;
    {
        cds_rcu_gpb gpb;                                  // RCU singleton
        cds::threading::Manager::attachThread();          // attach main thread
        {
            if (p.lock_name == "std")         run_avl_bench<std::mutex>(p, "avl-std");
            else if (p.lock_name == "tas")    run_avl_bench<tas_lock>(p, "avl-tas");
            else if (p.lock_name == "ttas")   run_avl_bench<ttas_lock>(p, "avl-ttas");
            else if (p.lock_name == "cas")    run_avl_bench<cas_lock>(p, "avl-cas");
            else if (p.lock_name == "ticket") run_avl_bench<ticket_lock>(p, "avl-ticket");
            else {
                std::cerr << "Unsupported --lock " << p.lock_name
                          << " (use std|tas|ttas|cas|ticket)\n";
                rc = 2;
            }
        }
        cds::threading::Manager::detachThread();
    }
    cds::Terminate();
    return rc;
}
