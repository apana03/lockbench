#pragma once
// Per-thread pre-rolled (key, op_code) streams. Walked cyclically by the
// benchmark hot loop so that per-op RNG / Zipfian sampling cost (~15-80 ns/op
// in the live-RNG path) is moved out of the measurement window. The stream
// is built once per worker using the same RNGs and op-mix logic as the live
// generator, so the empirical workload distribution is statistically
// indistinguishable from the original within the period.
//
// Every lock variant in a sweep consumes the SAME fixed stream, so lock-vs-
// lock comparisons remain fair. Any empirical-vs-theoretical drift at small
// N is shared bias.
//
// See docs/INDEX_LOCK_DECISIONS.md D22 for context.

#include <cstdint>
#include <random>
#include <vector>

#include "zipfian.hpp"

struct alignas(16) op_entry {
  std::uint64_t key;
  std::uint8_t  op;   // 0 = get, 1 = put, 2 = remove
};

// Round n up to the next power of two. Clamps min to 64 to keep statistical
// fidelity acceptable even for callers that pass a tiny value.
inline std::size_t round_up_pow2(std::size_t n) noexcept {
  if (n < 64) n = 64;
  std::size_t p = 1;
  while (p < n) p <<= 1;
  return p;
}

// Resolve a 0..99 op draw into the {0,1,2} op code under cumulative thresholds.
inline std::uint8_t resolve_op(int draw, int read_pct, int insert_pct) noexcept {
  if (draw < read_pct)                       return 0;
  if (draw < read_pct + insert_pct)          return 1;
  return 2;
}

// Uniform-key stream: keys are drawn uniformly from [0, key_range). Uses the
// same mt19937_64 + uniform_int_distribution and mt19937 + uniform_int(0,99)
// pair as the live path in run_bench_common.
inline std::vector<op_entry> make_stream_uniform(std::uint64_t key_range,
                                                 int read_pct, int insert_pct,
                                                 std::size_t n,
                                                 std::uint64_t key_seed,
                                                 std::uint64_t op_seed) {
  std::vector<op_entry> out(n);
  std::mt19937_64 key_rng(key_seed);
  std::uniform_int_distribution<std::uint64_t> key_dist(0, key_range - 1);
  std::mt19937 op_rng(static_cast<std::uint32_t>(op_seed));
  std::uniform_int_distribution<int> op_dist(0, 99);
  for (std::size_t i = 0; i < n; ++i) {
    out[i].key = key_dist(key_rng);
    out[i].op  = resolve_op(op_dist(op_rng), read_pct, insert_pct);
  }
  return out;
}

// Zipfian-key stream: keys come from zipfian_generator::next_scrambled, so
// the marginal distribution matches the live path exactly. Op selection uses
// the same mt19937 + uniform_int(0,99) as live.
inline std::vector<op_entry> make_stream_zipfian(std::uint64_t key_range,
                                                 double theta,
                                                 int read_pct, int insert_pct,
                                                 std::size_t n,
                                                 std::uint64_t key_seed,
                                                 std::uint64_t op_seed) {
  std::vector<op_entry> out(n);
  zipfian_generator gen(key_range, theta, key_seed);
  std::mt19937 op_rng(static_cast<std::uint32_t>(op_seed));
  std::uniform_int_distribution<int> op_dist(0, 99);
  for (std::size_t i = 0; i < n; ++i) {
    out[i].key = gen.next_scrambled();
    out[i].op  = resolve_op(op_dist(op_rng), read_pct, insert_pct);
  }
  return out;
}
