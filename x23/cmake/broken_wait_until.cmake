include(CheckCXXSourceRuns)

check_cxx_source_runs([=[
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

struct testclock {
  using rep = std::uint64_t;
  using period = std::nano;
  using duration = std::chrono::duration<rep, period>;
  using time_point = std::chrono::time_point<testclock>;
  static constexpr bool is_steady = true;

  static time_point
  now() noexcept
  {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
    return time_point(duration(ns));
  }
};

int
main()
{
  std::mutex m;
  std::condition_variable cv;
  std::atomic_bool entered{false};
  std::atomic_bool stop{false};
  std::atomic_int returns{0};

  std::thread waiter([&] {
    std::unique_lock lk(m);
    entered.store(true, std::memory_order_release);
    entered.notify_all();
    while (!stop.load(std::memory_order_acquire)) {
      cv.wait_until(lk, testclock::time_point::max());
      returns.fetch_add(1, std::memory_order_relaxed);
    }
  });

  while (!entered.load(std::memory_order_acquire))
    entered.wait(false);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  bool broken = returns.load(std::memory_order_relaxed) != 0;
  stop.store(true, std::memory_order_release);
  cv.notify_all();
  waiter.join();
  return broken ? 1 : 0;
}
]=] HAVE_WORKING_WAIT_UNTIL_MAX)

if(HAVE_WORKING_WAIT_UNTIL_MAX)
  set(HAVE_BROKEN_WAIT_UNTIL 0 CACHE INTERNAL
    "condition_variable::wait_until handles unsigned custom-clock max")
else()
  set(HAVE_BROKEN_WAIT_UNTIL 1 CACHE INTERNAL
    "condition_variable::wait_until mishandles unsigned custom-clock max")
endif()
