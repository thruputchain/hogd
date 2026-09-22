// -*- C++ -*-

// The clock's representation is load-bearing: an unsigned rep turns any
// out-of-order pair of samples into a ~584-year interval rather than a small
// negative one.  Every elapsed-time consumer subtracts two samples, so these
// tests pin the property down at the clock rather than at each sink.

#include <chrono>
#include <cstdint>
#include <limits>
#include <print>
#include <thread>
#include <type_traits>

#include "x23/debug.h"
#include "x23/fastclock.h"

using namespace std::chrono_literals;
using x23::fastclock;

static void
progress(const char *msg)
{
  std::println(stderr, "fastclock: {}", msg);
  std::fflush(stderr);
}

// A signed rep is what makes a regressing sample merely negative.  Callers
// that want a raw counter still get an unsigned one from get_ns(), which is
// why this has to be checked on the duration and not on the clock's API.
static void
test_rep_is_signed()
{
  static_assert(std::is_signed_v<fastclock::duration::rep>);
  static_assert(
      std::chrono::treat_as_floating_point_v<fastclock::duration::rep> ==
      false);
  x23assert(std::is_signed_v<fastclock::duration::rep>);
  progress("done rep_is_signed");
}

// The property the sinks depend on: subtracting a later-but-lower sample
// yields a small negative interval, not a wrapped positive one.  A sink that
// formats this into a log prints "-60", which reads as the clock glitch it is.
static void
test_regressing_sample_stays_negative()
{
  auto first = fastclock::time_point(fastclock::duration(100));
  auto regressed = fastclock::time_point(fastclock::duration(40));

  auto elapsed = regressed - first;
  x23assert(elapsed.count() == -60);
  x23assert(elapsed < fastclock::duration::zero());

  // Same bits, different reading: an unsigned rep would have handed the sinks
  // these as a ~584-year interval.  The signed rep is the whole of what keeps
  // that number out of the logs, so pin down that the bits really are the
  // wrapped ones and only the interpretation differs.
  x23assert(static_cast<std::uint64_t>(elapsed.count()) ==
            std::numeric_limits<std::uint64_t>::max() - 59);
  x23assert(std::chrono::abs(elapsed) < 1s);
  progress("done regressing_sample_stays_negative");
}

// now() narrows get_ns()'s unsigned counter into the signed rep.  A TSC-derived
// nanosecond count is nowhere near the 292-year range of an int64_t, so the
// narrowing has to be exact rather than wrapping into the sign bit.
static void
test_now_agrees_with_get_ns()
{
  std::uint64_t const raw = fastclock::get_ns();
  auto const tp = fastclock::now();

  x23assert(tp.time_since_epoch().count() > 0);
  x23assert(raw <= static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max()));

  auto const skew = tp.time_since_epoch() -
                    fastclock::duration(static_cast<std::int64_t>(raw));
  x23assert(skew >= fastclock::duration::zero());
  x23assert(skew < std::chrono::seconds(1));
  progress("done now_agrees_with_get_ns");
}

// An ordinary forward-going measurement still has to come out positive and in
// the right ballpark -- the signed rep must not have cost the clock its range.
static void
test_elapsed_measurement_is_sane()
{
  auto const start = fastclock::now();
  std::this_thread::sleep_for(20ms);
  auto const stop = fastclock::now();

  auto const elapsed = stop - start;
  x23assert(elapsed > fastclock::duration::zero());
  // Generous upper bound: this only has to catch a badly mis-scaled counter,
  // not schedule noise on a loaded builder.
  x23assert(elapsed >= 1ms);
  x23assert(elapsed < 10s);
  progress("done elapsed_measurement_is_sane");
}

int
main()
{
  progress("start main");
  test_rep_is_signed();
  test_regressing_sample_stays_negative();
  test_now_agrees_with_get_ns();
  test_elapsed_measurement_is_sane();
  progress("done main");
  return 0;
}
