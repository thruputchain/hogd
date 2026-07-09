// -*- C++ -*-

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <print>
#include <thread>
#include <utility>
#include <vector>

#include "x23/corosleep.h"
#include "x23/debug.h"
#include "x23/threadpool.h"

using namespace std::chrono_literals;
using namespace x23;

using Clock = CoroSleep::Clock;

constexpr auto kMaxOversleep = 2s;

static void
progress(const char *msg)
{
  std::println(stderr, "corosleep: {}", msg);
  std::fflush(stderr);
}

struct SleepResult {
  bool active = false;
  Clock::time_point start;
  Clock::time_point stop;
};

static Async<SleepResult>
record_sleep(CoroSleep &cs, std::chrono::nanoseconds duration)
{
  SleepResult result;
  result.start = Clock::now();
  result.active = co_await cs.sleep(duration);
  result.stop = Clock::now();
  co_return result;
}

static Async<SleepResult>
record_sleep_until(CoroSleep &cs, CoroSleep::Clock::time_point deadline)
{
  SleepResult result;
  result.start = Clock::now();
  result.active = co_await cs.sleep_until(deadline);
  result.stop = Clock::now();
  co_return result;
}

static void
expect_elapsed_at_least(const SleepResult &result,
                        std::chrono::nanoseconds duration)
{
  auto elapsed = result.stop - result.start;
  x23assert(result.active);
  x23assert(elapsed >= duration);
  x23assert(elapsed < duration + kMaxOversleep);
}

static void
test_repeated_relative_sleep()
{
  progress("start repeated_relative_sleep");
  ThreadPool pool(4);
  CoroSleep cs(pool);

  for (int i = 0; i < 20; i++) {
    SleepResult result = record_sleep(cs, 10ms)();
    expect_elapsed_at_least(result, 10ms);
  }
  progress("done repeated_relative_sleep");
}

static void
test_expired_deadline_wakes_promptly()
{
  progress("start expired_deadline_wakes_promptly");
  ThreadPool pool(4);
  CoroSleep cs(pool);

  SleepResult result = record_sleep_until(cs, CoroSleep::Clock::now() - 1ms)();

  auto elapsed = result.stop - result.start;
  x23assert(result.active);
  x23assert(elapsed < 250ms);
  progress("done expired_deadline_wakes_promptly");
}

static void
test_tiny_sleep_deadlines_do_not_hang()
{
  progress("start tiny_sleep_deadlines_do_not_hang");
  ThreadPool pool(4);
  CoroSleep cs(pool);

  for (int i = 0; i < 500; i++) {
    if (i % 100 == 0)
      progress("tiny_sleep_deadlines_do_not_hang loop");
    SleepResult result = record_sleep(cs, 1ns)();
    expect_elapsed_at_least(result, 1ns);
  }
  progress("done tiny_sleep_deadlines_do_not_hang");
}

static void
test_batches_many_waiters()
{
  progress("start batches_many_waiters");
  ThreadPool pool(4);
  CoroSleep cs(pool);

  constexpr int kWaiters = 128;
  std::vector<SleepResult> results;
  std::vector<Async<SleepResult>> sleepers;
  results.reserve(kWaiters);
  sleepers.reserve(kWaiters);

  for (int i = 0; i < kWaiters; i++)
    sleepers.emplace_back(record_sleep(cs, 15ms));
  for (auto &sleeper : sleepers)
    results.emplace_back(sleeper());

  for (const auto &result : results)
    expect_elapsed_at_least(result, 15ms);
  progress("done batches_many_waiters");
}

static void
test_sleep_until_orders_deadlines()
{
  progress("start sleep_until_orders_deadlines");
  ThreadPool pool(4);
  CoroSleep cs(pool);

  constexpr int kWaiters = 12;
  auto base = CoroSleep::Clock::now() + 20ms;
  std::vector<SleepResult> results(kWaiters);
  std::vector<std::pair<int, Async<SleepResult>>> sleepers;
  sleepers.reserve(kWaiters);

  for (int i = kWaiters - 1; i >= 0; i--)
    sleepers.emplace_back(i, record_sleep_until(cs, base + i * 8ms));
  for (auto &[index, sleeper] : sleepers)
    results[index] = sleeper();

  for (const auto &result : results)
    x23assert(result.active);
  for (int i = 1; i < kWaiters; i++)
    x23assert(results[i - 1].stop <= results[i].stop + 5ms);
  progress("done sleep_until_orders_deadlines");
}

static void
test_earlier_insert_rechecks_service_thread()
{
  progress("start earlier_insert_rechecks_service_thread");
  ThreadPool pool(4);
  CoroSleep cs(pool);

  auto slow_sleep = record_sleep(cs, 300ms);

  std::this_thread::sleep_for(25ms);
  auto fast_sleep = record_sleep(cs, 20ms);

  SleepResult fast = fast_sleep();
  SleepResult slow = slow_sleep();

  expect_elapsed_at_least(fast, 20ms);
  expect_elapsed_at_least(slow, 300ms);
  x23assert(fast.stop < slow.stop);
  progress("done earlier_insert_rechecks_service_thread");
}

static Async<void>
sleep_until_destroyed(CoroSleep &cs, unsigned char &active,
                      std::atomic_int &completed)
{
  active = co_await cs.sleep(1h);
  completed.fetch_add(1, std::memory_order_release);
  completed.notify_all();
}

static void
test_destroy_wakes_waiters_false()
{
  progress("start destroy_wakes_waiters_false");
  ThreadPool pool(4);
  auto cs = std::make_unique<CoroSleep>(pool);

  constexpr int kWaiters = 64;
  std::vector<unsigned char> active(kWaiters, true);
  std::atomic_int completed{0};
  std::vector<Async<void>> sleepers;
  sleepers.reserve(kWaiters);

  for (int i = 0; i < kWaiters; i++)
    sleepers.emplace_back(sleep_until_destroyed(*cs, active[i], completed));

  std::this_thread::sleep_for(25ms);
  cs.reset();

  int old_completed = completed.load(std::memory_order_acquire);
  while (old_completed != kWaiters) {
    completed.wait(old_completed);
    old_completed = completed.load(std::memory_order_acquire);
  }

  for (auto &sleeper : sleepers)
    sleeper();
  for (unsigned char value : active)
    x23assert(!value);
  progress("done destroy_wakes_waiters_false");
}

int
main()
{
  progress("start main");
  test_repeated_relative_sleep();
  test_expired_deadline_wakes_promptly();
  test_tiny_sleep_deadlines_do_not_hang();
  test_batches_many_waiters();
  test_sleep_until_orders_deadlines();
  test_earlier_insert_rechecks_service_thread();
  test_destroy_wakes_waiters_false();
  progress("done main");
  return 0;
}
