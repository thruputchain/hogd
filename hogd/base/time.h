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
  using clock = std::chrono::high_resolution_clock;

  // a timestamp for when the timer was created.
  clock::time_point start_ts_;
};

} // namespace hogd
