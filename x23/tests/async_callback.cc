// GTest for Async<R>::operator()(callback).
//
// Invoking an Async with a callback used to be a coroutine whose
// initial_suspend() did not suspend, so its body ran inside its own ramp
// function.  When the result arrived from another thread, that thread
// resumed the body and ran the frame's completion path concurrently with
// the ramp, which was still decrementing the same non-atomic frame use
// count -- the losing interleavings double-freed (or leaked) the frame.
// The cross-thread tests below are the shape that raced; the symptom is
// heap corruption, so they only mean anything under ASAN/TSAN.

#include <atomic>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "x23/async.h"
#include "x23/threadpool.h"

using namespace x23;

namespace {

ThreadPool pool;

constexpr int kIters = 1000;

} // namespace

// The result is produced by another thread, racing the invocation itself.
TEST(AsyncCallback, CrossThreadCompletion)
{
  for (int i = 0; i < kIters; i++) {
    auto [async, cb] = make_async<int>();
    std::atomic_bool go{false};
    std::atomic_int got{-1};

    std::thread producer([cb, &go] {
      while (!go.load(std::memory_order_acquire)) {
      }
      cb(42);
    });

    go.store(true, std::memory_order_release);
    std::move(async)(
        [&got](int v) { got.store(v, std::memory_order_release); });

    producer.join();
    EXPECT_EQ(got.load(std::memory_order_acquire), 42);
  }
}

// Same race with a void result, which takes the other branch of the
// callback invocation.
TEST(AsyncCallback, CrossThreadCompletionVoid)
{
  for (int i = 0; i < kIters; i++) {
    auto [async, cb] = make_async<void>();
    std::atomic_bool go{false};
    std::atomic_int calls{0};

    std::thread producer([cb, &go] {
      while (!go.load(std::memory_order_acquire)) {
      }
      cb();
    });

    go.store(true, std::memory_order_release);
    std::move(async)(
        [&calls] { calls.fetch_add(1, std::memory_order_acq_rel); });

    producer.join();
    EXPECT_EQ(calls.load(std::memory_order_acquire), 1);
  }
}

// Results produced on pool threads, with many invocations in flight at
// once, so the resumes come from threads this one never synchronizes with
// until the callbacks fire.
TEST(AsyncCallback, PoolCompletion)
{
  std::atomic_int done{0};

  for (int i = 0; i < kIters; i++) {
    auto [async, cb] = make_async<int>();

    [](CoroCB<int> cb, int v) -> Detached {
      co_await pool.bg();
      cb(v);
    }(cb, i);

    std::move(async)([&done, i](int v) {
      EXPECT_EQ(v, i);
      done.fetch_add(1, std::memory_order_acq_rel);
      done.notify_all();
    });
  }

  for (int seen = done.load(std::memory_order_acquire); seen < kIters;
       seen = done.load(std::memory_order_acquire))
    done.wait(seen);
  EXPECT_EQ(done.load(std::memory_order_acquire), kIters);
}

// An already-ready result still calls back, synchronously, on this thread.
TEST(AsyncCallback, ReadyResultCallsBackInline)
{
  int got = -1;
  [](int n) -> Async<int> { co_return n + 5; }(2)([&got](int v) { got = v; });
  EXPECT_EQ(got, 7);

  int calls = 0;
  [] -> Async<void> { co_return; }()([&calls] { calls++; });
  EXPECT_EQ(calls, 1);
}

// Both callback overloads work: a bare callable and a Callback<R>.
TEST(AsyncCallback, AcceptsCallbackAndCallable)
{
  int got = -1;
  [] -> Async<int> {
    co_return 3;
  }()(Callback<int>{[&got](int v) { got = v; }});
  EXPECT_EQ(got, 3);

  got = -1;
  [] -> Async<int> { co_return 4; }()([&got](int v) { got = v; });
  EXPECT_EQ(got, 4);
}
