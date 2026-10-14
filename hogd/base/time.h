// time.h - utilities for measuring time

#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <string>

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

// UtcTime is a system_clock time point broken into UTC calendar fields.
//
// Format these fields as plain integers rather than passing a time point to
// std::format with a chrono spec ("{:%H:%M:%S}"): GCC 15's chrono formatter
// calls libstdc++ symbols that only exist in libstdc++ 15 (GLIBCXX_3.4.34),
// which would make every binary require it at runtime.
struct UtcTime {
  using duration = std::chrono::system_clock::duration;

  // Number of decimal digits in [subseconds] (9 for nanosecond clocks).
  static constexpr unsigned kSubsecondDigits =
      std::chrono::hh_mm_ss<duration>::fractional_width;

  int year;
  unsigned month;
  unsigned day;
  std::int64_t hours;
  std::int64_t minutes;
  std::int64_t seconds;
  std::int64_t subseconds;

  explicit UtcTime(std::chrono::system_clock::time_point tp)
  {
    const auto day_start = std::chrono::floor<std::chrono::days>(tp);
    const std::chrono::year_month_day ymd{day_start};
    const std::chrono::hh_mm_ss<duration> hms{tp - day_start};
    year = static_cast<int>(ymd.year());
    month = static_cast<unsigned>(ymd.month());
    day = static_cast<unsigned>(ymd.day());
    hours = hms.hours().count();
    minutes = hms.minutes().count();
    seconds = hms.seconds().count();
    subseconds = hms.subseconds().count();
  }

  // Returns ISO 8601 with full clock precision, e.g.
  // "2026-10-09T19:49:59.046318862Z".
  std::string iso8601() const
  {
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:0{}}Z", year,
                       month, day, hours, minutes, seconds, subseconds,
                       kSubsecondDigits);
  }
};

} // namespace hogd
