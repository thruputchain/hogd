// -*- C++ -*-

// Using the cycle counter and calibrating it to nanoseconds seems
// about 3 times faster than std::chrono::steady_clock.  On one x86-64
// machine, fastclock::now costs about 12ns compareed to 37ns for
// std::chrono::steady_clock::now().

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#ifndef FASTCLOCK_USE_TSC
#if (defined(__x86_64__) || defined(__i386__) || defined(__aarch64__)) && defined(__SIZEOF_INT128__)
#define FASTCLOCK_USE_TSC 1
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif // x86
#else // !(x86 || arm)
#define FASTCLOCK_USE_TSC 0
#endif // !(x86 || arm)
#endif // !FASTCLOCK_USE_TSC

namespace x23 {

#if FASTCLOCK_USE_TSC

struct fastclock {
  using rep = std::uint64_t;
  using period = std::nano;
  using duration = std::chrono::duration<rep, period>;
  using time_point = std::chrono::time_point<fastclock>;
  static constexpr bool is_steady = true;

private:
  static inline std::atomic_flag calibration_lock = ATOMIC_FLAG_INIT;
  static inline std::atomic<bool> calibrated{false};

  // nanoseconds =  (read_counter() * multiplier) >> shift
  static inline uint64_t multiplier;
  static constexpr unsigned shift = 32;

  static rep read_counter()
  {
#if defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#elif defined(__aarch64__)
    // According to ChatGPT (untested):
    uint64_t val;
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
#error "unsupported architecture"
#endif
  }

  static void calibrate()
  {
    if (calibration_lock.test_and_set()) {
      if (calibrated.load(std::memory_order_relaxed))
	calibrated.wait(false);
      return;
    }

    using std::uint64_t;

    auto t0 = std::chrono::steady_clock::now();
    uint64_t c0 = read_counter();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    uint64_t c1 = read_counter();
    auto t1 = std::chrono::steady_clock::now();

    uint64_t ticks = c1 - c0;
    if (ticks == 0)
      ticks = 1;

    uint64_t ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    if (ns == 0)
      ns = 1;

    multiplier = (static_cast<__uint128_t>(ns) << shift) / ticks;

    calibrated.store(true, std::memory_order_release);
    calibrated.notify_all();
  }

public:
  static time_point now() noexcept
  {
    if (!calibrated.load(std::memory_order_acquire)) [[unlikely]]
      calibrate();

    uint64_t tsc = read_counter();
    __uint128_t scaled = static_cast<__uint128_t>(tsc) * multiplier;
    auto ns = static_cast<uint64_t>(scaled >> shift);
    return time_point(duration(ns));
  }
};

#else // !FASTCLOCK_USE_TSC

using fastclock = std::chrono::steady_clock;

#endif // !FASTCLOCK_USE_TSC

} // namespace x23
