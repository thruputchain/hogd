// zipf.h - skewed rank generation for benchmarks.
//
// Shared so that benchmarks in different binaries draw from an identical
// distribution; results from ramdb_bench and hogdb_bench are only comparable if
// the generator is the same one.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hogd {

// next_rand advances a splitmix64 state and returns the next value. Each worker
// keeps its own state, so draws need no synchronization.
inline uint64_t
next_rand(uint64_t &state)
{
  state += 0x9E3779B97F4A7C15ULL;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// next_double returns a value in [0, 1) with a full 53-bit mantissa.
inline double
next_double(uint64_t &state)
{
  return static_cast<double>(next_rand(state) >> 11) * 0x1.0P-53;
}

// zeta returns the generalized harmonic number sum(1/i^theta) for i in [1, n].
inline double
zeta(uint64_t n, double theta)
{
  if (theta == 0.0)
    return static_cast<double>(n);

  double sum = 0.0;
  for (uint64_t i = 1; i <= n; ++i)
    sum += 1.0 / std::pow(static_cast<double>(i), theta);
  return sum;
}

// Zipf draws ranks from a Zipf distribution over [0, n) in constant time using
// the method of Gray et al., as used by YCSB. Rank 0 is the hottest. theta == 0
// yields a uniform distribution.
class Zipf {
public:
  Zipf(uint64_t n, double theta) : n_(n)
  {
    // theta == 1 is a pole in alpha_.
    theta_ = std::clamp(theta, 0.0, 0.999);
    zetan_ = zeta(n_, theta_);
    alpha_ = 1.0 / (1.0 - theta_);
    eta_ = (1.0 - std::pow(2.0 / static_cast<double>(n_), 1.0 - theta_)) /
           (1.0 - zeta(2, theta_) / zetan_);
    half_pow_ = std::pow(0.5, theta_);
  }

  uint32_t operator()(uint64_t &state) const
  {
    double u = next_double(state);
    double uz = u * zetan_;
    if (uz < 1.0)
      return 0;
    if (uz < 1.0 + half_pow_)
      return 1;

    auto rank = static_cast<uint64_t>(
        static_cast<double>(n_) * std::pow((eta_ * u) - eta_ + 1.0, alpha_));
    return static_cast<uint32_t>(std::min(rank, n_ - 1));
  }

private:
  uint64_t n_;
  double theta_;
  double alpha_;
  double eta_;
  double zetan_;
  double half_pow_;
};

} // namespace hogd
