// time.h - utilities for measuring time

#pragma once

#include <chrono>

namespace hogd {

class ElapsedTimer {
public:
  ElapsedTimer() : start_ts_(clock::now()) {}
  ~ElapsedTimer() = default;

  // elapsed returns the seconds that have elapsed since the timer was created.
  double elapsed() const
  {
    clock::time_point now_ts = clock::now();
    return std::chrono::duration<double>(now_ts - start_ts_).count();
  }

private:
  // steady_clock is monotonic, so elapsed() can never go backwards;
  // high_resolution_clock is not guaranteed steady (it aliases system_clock on
  // libstdc++) and could yield a negative interval on a wall-clock step.
  using clock = std::chrono::steady_clock;

  // a timestamp for when the timer was created.
  clock::time_point start_ts_;
};

} // namespace hogd
