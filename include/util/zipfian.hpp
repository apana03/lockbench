#pragma once
#include <cmath>
#include <cstdint>
#include <random>

// Zipfian distribution - generates skewed random values
// theta close to 0 is basically uniform, 0.99 is very skewed (most YCSB benchmarks use this)
class zipfian_generator {
public:
  zipfian_generator(std::uint64_t n, double theta, std::uint64_t seed = 42)
      : n_(n), theta_(theta), rng_(seed), uniform_(0.0, 1.0) {
    zetan_ = zeta(n_);
    eta_   = (1.0 - std::pow(2.0 / static_cast<double>(n_), 1.0 - theta_)) /
             (1.0 - zeta(2) / zetan_);
    alpha_ = 1.0 / (1.0 - theta_);
  }

  std::uint64_t next() noexcept {
    double u = uniform_(rng_);
    double uz = u * zetan_;

    if (uz < 1.0) return 0;
    if (uz < 1.0 + std::pow(0.5, theta_)) return 1;

    return static_cast<std::uint64_t>(
        static_cast<double>(n_) *
        std::pow(eta_ * u - eta_ + 1.0, alpha_));
  }

  // hash the output so hot keys aren't all bunched at the start
  std::uint64_t next_scrambled() noexcept {
    std::uint64_t v = next();
    v = fnv_hash(v);
    return v % n_;
  }

private:
  std::uint64_t n_;
  double theta_;
  double zetan_;
  double eta_;
  double alpha_;
  std::mt19937_64 rng_;
  std::uniform_real_distribution<double> uniform_;

  double zeta(std::uint64_t count) const noexcept {
    double sum = 0.0;
    for (std::uint64_t i = 1; i <= count; ++i) {
      sum += 1.0 / std::pow(static_cast<double>(i), theta_);
    }
    return sum;
  }

  static std::uint64_t fnv_hash(std::uint64_t val) noexcept {
    std::uint64_t h = 14695981039346656037ULL;
    for (int i = 0; i < 8; ++i) {
      h ^= (val & 0xFF);
      h *= 1099511628211ULL;
      val >>= 8;
    }
    return h;
  }
};
