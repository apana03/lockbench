// wh_bench - Wu et al.'s Wormhole index with a compile-time-fixed lock
// primitive. One source compiled into 7 binaries (wh_bench_default,
// wh_bench_rw, wh_bench_tas, ..., wh_bench_occ). Each binary's lock is
// selected by the linked wormhole-rt-<lk> static lib's compile defines.

#include <cstdio>
#include <iostream>
#include <string>

#include "../include/indexes/wormhole_index.hpp"
#include "../include/util/bench_harness.hpp"

#ifndef WH_LOCK_LABEL
#define WH_LOCK_LABEL "wh-unknown"
#endif

int main(int argc, char** argv) {
    params p = parse_bench_args(argc, argv);

    // Each binary's lock is fixed; --lock is accepted but ignored with a
    // warning if it disagrees with the compiled-in label.
    const char* expected = WH_LOCK_LABEL + 3;  // skip "wh-"
    if (!p.lock_name.empty() && p.lock_name != expected) {
        std::fprintf(stderr,
            "warning: this binary is %s, ignoring --lock %s\n",
            WH_LOCK_LABEL, p.lock_name.c_str());
    }

    wormhole_index index;
    prefill_index(index, p);

    run_bench_common(p, WH_LOCK_LABEL, index,
        [](auto& idx, std::uint64_t key) { idx.get(key); },
        [](auto& idx, std::uint64_t key) { idx.put(key, key + 1); },
        [](auto& idx, std::uint64_t key) { idx.remove(key); });
    return 0;
}
