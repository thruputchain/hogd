// -*-C++-*-

#pragma once

#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdint>

template<typename Unit = std::chrono::nanoseconds,
         std::floating_point R = double>
class LatencyStats {
public:
  using Rep = R;
  using Period = Unit::period;
  using Dur = std::chrono::duration<Rep, Period>;

  // Record one latency sample
  template<typename Rep, typename Period>
  void sample(std::chrono::duration<Rep, Period> d) noexcept
  {
    auto x = Dur{d}.count();

    // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Welford's_online_algorithm
    ++n_;
    const auto delta = x - mean_;
    mean_ += delta / n_;
    const auto delta2 = x - mean_;
    m2_ += delta * delta2;
  }

  // Number of samples seen.
  std::uint64_t count() const noexcept { return n_; }

  // Mean latency, in Unit (returns 0 if no samples).
  Dur mean() const noexcept { return Dur{n_ ? mean_ : 0}; }

  // Unbiased standard deviation (n-1 in the denominator).
  // Returns 0 if fewer than 2 samples.
  Dur stddev() const noexcept
  {
    if (n_ < 2)
      return Dur{};
    auto var = m2_ / (n_ - 1);
    return Dur{std::sqrt(var)};
  }

  void reset() noexcept
  {
    n_ = 0;
    m2_ = mean_ = 0;
  }

  // Merge another LatencyStats into this one.
  LatencyStats &operator+=(const LatencyStats &o) noexcept
  {
    if (o.n_ == 0)
      return *this;
    if (n_ == 0) {
      *this = o;
      return *this;
    }

    const auto new_n = n_ + o.n_;
    const auto w = Rep(o.n_) / new_n;
    const auto delta = o.mean_ - mean_;

    mean_ += w * delta;
    m2_ += o.m2_ + (delta * delta) * (n_ * w);

    n_ = new_n;
    return *this;
  }

  friend LatencyStats operator+(LatencyStats a, const LatencyStats &b) noexcept
  {
    a += b;
    return a;
  }

private:
  std::uint64_t n_{}; // sample count
  Rep mean_{};        // running mean
  Rep m2_{};          // sum of squares of diffs from mean_
};
