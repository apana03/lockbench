// wh_test - correctness probe for Wu et al.'s Wormhole with a compile-time-
// fixed lock primitive. Mirrors cds_test/cds_avl_test structure.
//   --mode single : random put/get/remove against std::map oracle
//   --mode race   : N threads on disjoint key slices

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../include/primitives/util.hpp"
#include "../include/indexes/wormhole_index.hpp"

#ifndef WH_LOCK_LABEL
#define WH_LOCK_LABEL "wh-unknown"
#endif

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "FAIL: " << (msg) << " at " << __FILE__ \
                                  << ":" << __LINE__ << "\n"; ++failures; } } while (0)

static void single_threaded_oracle(wormhole_index& idx, std::uint64_t key_range,
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
    for (auto& [k, v] : oracle) {
        auto got = idx.get(k);
        CHECK(got.has_value() && *got == v, "final sweep: oracle key missing");
    }
}

static void race_test(wormhole_index& idx, int threads,
                      std::uint64_t per_thread_keys, std::uint64_t ops_per_thread) {
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

struct cfg {
    std::string mode = "both";
    int threads = 8;
    std::uint64_t key_range       = 1'000;
    std::uint64_t ops             = 50'000;
    std::uint64_t per_thread_keys = 256;
    std::uint64_t ops_per_thread  = 20'000;
    std::uint32_t seed            = 42;
};

static cfg parse(int argc, char** argv) {
    cfg c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) { std::cerr << "Missing value for " << name << "\n"; std::exit(2); }
            return std::string(argv[++i]);
        };
        if (a == "--mode")              c.mode = need("--mode");
        else if (a == "--threads")      c.threads = std::stoi(need("--threads"));
        else if (a == "--key_range")    c.key_range = std::stoull(need("--key_range"));
        else if (a == "--ops")          c.ops = std::stoull(need("--ops"));
        else if (a == "--per_thread_keys") c.per_thread_keys = std::stoull(need("--per_thread_keys"));
        else if (a == "--ops_per_thread")  c.ops_per_thread = std::stoull(need("--ops_per_thread"));
        else if (a == "--seed")         c.seed = static_cast<std::uint32_t>(std::stoul(need("--seed")));
        else if (a == "--lock")         { (void)need("--lock"); }  // accepted, ignored
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: wh_test_<lock> [--mode single|race|both]\n"
                      << "                       [--threads N] [--key_range N] [--ops N]\n"
                      << "                       [--per_thread_keys N] [--ops_per_thread N] [--seed S]\n";
            std::exit(0);
        } else { std::cerr << "Unknown arg: " << a << "\n"; std::exit(2); }
    }
    return c;
}

int main(int argc, char** argv) {
    cfg c = parse(argc, argv);

    std::cout << "[" << WH_LOCK_LABEL << "]\n";
    int before = failures;
    if (c.mode == "single" || c.mode == "both") {
        wormhole_index idx;
        single_threaded_oracle(idx, c.key_range, c.ops, c.seed);
        std::cout << "  single-threaded oracle: "
                  << (failures == before ? "OK" : "FAIL") << "\n";
    }
    int mid = failures;
    if (c.mode == "race" || c.mode == "both") {
        wormhole_index idx;
        race_test(idx, c.threads, c.per_thread_keys, c.ops_per_thread);
        std::cout << "  race ("
                  << c.threads << " threads x " << c.ops_per_thread << " ops): "
                  << (failures == mid ? "OK" : "FAIL") << "\n";
    }

    std::cout << "\n" << (failures ? "FAILED" : "PASSED")
              << " (" << failures << " check failure"
              << (failures == 1 ? "" : "s") << ")\n";
    return failures ? 1 : 0;
}
