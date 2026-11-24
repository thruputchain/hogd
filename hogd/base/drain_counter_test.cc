#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <semaphore>
#include <thread>
#include <vector>

#include "hogd/base/drain_counter.h"
#include "x23/async.h"

namespace hogd {

namespace {

// Starts a coroutine that waits for [c] to drain, then sets [resumed].
void
start_waiter(DrainCounter &c, bool &resumed)
{
  [](DrainCounter &c, bool &resumed) -> x23::Detached {
    co_await c.wait_idle();
    resumed = true;
  }(c, resumed);
}

} // namespace

TEST(DrainCounterTest, IdleCounterDoesNotSuspend)
{
  DrainCounter c;
  bool resumed = false;
  start_waiter(c, resumed);
  EXPECT_TRUE(resumed);
  EXPECT_EQ(c.in_flight(), 0U);
}

TEST(DrainCounterTest, LastDoneResumesWaiterInline)
{
  DrainCounter c;
  c.add();
  c.add();
  c.add();
  bool resumed = false;
  start_waiter(c, resumed);
  EXPECT_FALSE(resumed);
  c.done();
  c.done();
  EXPECT_FALSE(resumed);
  EXPECT_EQ(c.in_flight(), 1U);
  c.done();
  // Resumed by the done() call itself, before it returned.
  EXPECT_TRUE(resumed);
  EXPECT_EQ(c.in_flight(), 0U);
}

TEST(DrainCounterTest, OperationsAddedWhileWaitingAreAwaited)
{
  DrainCounter c;
  c.add();
  bool resumed = false;
  start_waiter(c, resumed);
  c.add();
  c.done();
  EXPECT_FALSE(resumed);
  c.done();
  EXPECT_TRUE(resumed);
}

TEST(DrainCounterTest, CanWaitAgainAfterADrain)
{
  DrainCounter c;
  for (int round = 0; round < 3; ++round) {
    c.add();
    bool resumed = false;
    start_waiter(c, resumed);
    EXPECT_FALSE(resumed) << "round " << round;
    c.done();
    EXPECT_TRUE(resumed) << "round " << round;
  }
}

TEST(DrainCounterTest, OperationsAfterADrainDoNotResumeAnything)
{
  DrainCounter c;
  c.add();
  bool resumed = false;
  start_waiter(c, resumed);
  c.done();
  ASSERT_TRUE(resumed);

  // Stragglers that start and finish with no waiter parked.
  c.add();
  c.done();
  c.add();
  c.add();
  c.done();
  c.done();

  bool resumed_again = false;
  start_waiter(c, resumed_again);
  EXPECT_TRUE(resumed_again);
}

// The waiter may destroy the counter as soon as it resumes, while the done()
// that resumed it is still on the stack.
TEST(DrainCounterTest, WaiterMayDestroyCounterOnResume)
{
  auto *c = new DrainCounter;
  c->add();
  c->add();
  bool destroyed = false;
  [](DrainCounter *c, bool &destroyed) -> x23::Detached {
    co_await c->wait_idle();
    delete c;
    destroyed = true;
  }(c, destroyed);
  c->done();
  EXPECT_FALSE(destroyed);
  c->done();
  EXPECT_TRUE(destroyed);
}

// Many threads retire operations while the waiter parks at an arbitrary point.
// The waiter frees the counter on resume, so a done() that touched the counter
// after handing off, or a lost or doubled wakeup, shows up here (most reliably
// under ASan or TSan).
TEST(DrainCounterTest, ConcurrentDrainThenDestroy)
{
  constexpr int kIterations = 5000;
  constexpr int kThreads = 4;
  constexpr int kOpsPerThread = 32;

  DrainCounter *current = nullptr;
  bool stop = false;
  std::barrier start_line(kThreads + 1);
  std::vector<std::jthread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      while (true) {
        start_line.arrive_and_wait();
        if (stop)
          return;
        DrainCounter *c = current;
        for (int i = 0; i < kOpsPerThread; ++i)
          c->done();
      }
    });
  }

  std::binary_semaphore resumed{0};
  std::atomic<int> resumes{0};
  auto start_waiter = [&](DrainCounter *c) {
    [](DrainCounter *c, std::atomic<int> &resumes,
       std::binary_semaphore &resumed) -> x23::Detached {
      co_await c->wait_idle();
      EXPECT_EQ(c->in_flight(), 0U);
      delete c;
      resumes.fetch_add(1);
      resumed.release();
    }(c, resumes, resumed);
  };

  for (int iter = 0; iter < kIterations; ++iter) {
    auto *c = new DrainCounter;
    for (int i = 0; i < kThreads * kOpsPerThread; ++i)
      c->add();
    current = c;
    // Park the waiter before the retirements start, or race it with them.
    if (iter % 2 == 0) {
      start_waiter(c);
      start_line.arrive_and_wait();
    }
    else {
      start_line.arrive_and_wait();
      start_waiter(c);
    }
    resumed.acquire();
    ASSERT_EQ(resumes.load(), iter + 1);
  }

  stop = true;
  start_line.arrive_and_wait();
}

// Operations keep starting and finishing, often from zero, while one waiter
// repeatedly drains: the straggler pattern, where the count goes back up after
// a wait completes.  Every wait must still be woken, exactly once.
TEST(DrainCounterTest, ConcurrentRepeatedWaitsWithStragglers)
{
  constexpr int kWaits = 5000;
  constexpr int kThreads = 3;

  DrainCounter c;
  std::atomic<bool> stop{false};
  std::vector<std::jthread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&c, &stop] {
      while (!stop.load(std::memory_order_relaxed)) {
        c.add();
        c.add();
        c.done();
        c.done();
      }
    });
  }

  std::binary_semaphore resumed{0};
  std::atomic<int> resumes{0};
  for (int i = 0; i < kWaits; ++i) {
    // Holding an operation of our own guarantees the waiter parks; releasing
    // it races the stragglers for who retires the last one.
    c.add();
    [](DrainCounter &c, std::atomic<int> &resumes,
       std::binary_semaphore &resumed) -> x23::Detached {
      co_await c.wait_idle();
      resumes.fetch_add(1);
      resumed.release();
    }(c, resumes, resumed);
    ASSERT_EQ(resumes.load(), i);
    c.done();
    resumed.acquire();
    ASSERT_EQ(resumes.load(), i + 1);
  }

  stop.store(true, std::memory_order_relaxed);
  threads.clear();
  EXPECT_EQ(c.in_flight(), 0U);
}

} // namespace hogd
