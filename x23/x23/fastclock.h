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
#include <type_traits>

#if defined(__APPLE__)
#define FASTCLOCK_USE_TSC 0
#endif // __APPLE__

#ifndef FASTCLOCK_USE_TSC
#if (defined(__x86_64__) || defined(__i386__) || defined(__aarch64__)) &&      \
    defined(__SIZEOF_INT128__)
#define FASTCLOCK_USE_TSC 1
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif // x86
#else  // !(x86 || arm)
#define FASTCLOCK_USE_TSC 0
#endif // !(x86 || arm)
#endif // !FASTCLOCK_USE_TSC

namespace x23 {

#if FASTCLOCK_USE_TSC

struct fastclock {
  // Signed, and it has to stay that way.  std::condition_variable's
  // wait_until reads the clock more than once -- it compares the deadline
  // against one read, then computes `deadline - now()` against a later one
  // -- so a deadline that expires in between produces a negative interval.
  // With an unsigned rep that subtraction wraps instead, and a wait of a
  // few nanoseconds becomes a wait of roughly 585 years.  Signed arithmetic
  // makes the expired deadline merely negative, which every implementation
  // treats as "already timed out".
  using rep = std::int64_t;
  static_assert(std::is_signed_v<rep>,
                "an unsigned rep turns every regressing sample into a "
                "~584-year interval, in wait_until and in elapsed-time "
                "telemetry alike");
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
  static uint64_t get_ns() noexcept
  {
    if (!calibrated.load(std::memory_order_acquire)) [[unlikely]]
      calibrate();

    uint64_t tsc = read_counter();
    __uint128_t scaled = static_cast<__uint128_t>(tsc) * multiplier;
    auto ns = static_cast<uint64_t>(scaled >> shift);
    return ns;
  }

  // get_ns stays unsigned: its other callers shift it to seed RNGs, where
  // running into the sign bit would be undefined.  A TSC-derived nanosecond
  // count is nowhere near the 292-year range of a signed rep, so narrowing
  // it here is exact.
  static time_point now() noexcept
  {
    return time_point(duration(static_cast<rep>(get_ns())));
  }
};

#else // !FASTCLOCK_USE_TSC

struct fastclock : std::chrono::steady_clock {
  static uint64_t get_ns() noexcept
  {
    return duration_cast<std::chrono::nanoseconds>(now().time_since_epoch())
        .count();
  }
};

#endif // !FASTCLOCK_USE_TSC

} // namespace x23
