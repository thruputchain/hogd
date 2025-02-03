#include <atomic>
#include <gtest/gtest.h>

#include "hogd/coord/coro_queue.h"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/threadpool.h"

TEST(CoroQueue, SingleThread)
{

  x23::ThreadPool pool(1);
  x23::CoroPoll cp(pool);

  hogd::CoroQueue queue(100, cp);

  std::atomic_flag done = false;

  std::atomic_uint32_t count = 0;
  std::atomic_uint32_t work_lambdas = 0;

  auto work = [&] -> x23::Async<void> {
    work_lambdas.fetch_add(1);
    while (true) {
      if (count.fetch_add(1) >= 10000) {
        done.test_and_set();
        co_return;
      }
    }
  };

  queue.enqueue(work, done)();

  EXPECT_TRUE(done.test());
  EXPECT_GE(count.load(), 10000);
  EXPECT_EQ(count.load(), work_lambdas.load() + 10000);
}

TEST(CoroQueue, MultiThread)
{
  x23::ThreadPool pool(10);
  x23::CoroPoll cp(pool);

  hogd::CoroQueue queue(100, cp);

  std::atomic_flag done = false;

  std::atomic_uint32_t count = 0;
  std::atomic_uint32_t work_lambdas = 0;

  auto work = [&] -> x23::Async<void> {
    work_lambdas.fetch_add(1);
    while (true) {
      if (count.fetch_add(1) >= 10000) {
        done.test_and_set();
        co_return;
      }
    }
  };

  queue.enqueue(work, done)();

  EXPECT_TRUE(done.test());
  EXPECT_GE(count.load(), 10000);
  EXPECT_EQ(count.load(), work_lambdas.load() + 10000);
}

TEST(CoroQueue, WorkThreadsThatFinishEarly)
{
  x23::ThreadPool pool(10);
  x23::CoroPoll cp(pool);

  hogd::CoroQueue queue(100, cp);

  std::atomic_flag done = false;

  std::atomic_uint32_t work_lambdas = 0;

  auto work = [&] -> x23::Async<void> {
    auto res = work_lambdas.fetch_add(1);

    if (res >= 1000) {
      done.test_and_set();
    }
    co_return;
  };

  queue.enqueue(work, done)();

  EXPECT_TRUE(done.test());
  EXPECT_GE(work_lambdas.load(), 1000);
}
