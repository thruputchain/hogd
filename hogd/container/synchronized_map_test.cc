#include <gtest/gtest.h>

#include <atomic>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hogd/container/synchronized_map.h"

namespace hogd {

TEST(SynchronizedMap, BasicUsage)
{
  SynchronizedMap<int, std::string> map({{10, "foo"}, {20, "bar"}});

  // check 10
  {
    auto [val, ul] = map[10];
    EXPECT_EQ(val, "foo");
  }

  // check 20
  {
    auto [val, ul] = map[20];
    EXPECT_EQ(val, "bar");
  }

  // modify 10
  {
    auto [val, ul] = map[10];
    val = "baz";
    EXPECT_EQ(val, "baz");
  }

  // recheck 10
  {
    auto [val, ul] = map[10];
    EXPECT_EQ(val, "baz");
  }

  // can't reinsert
  {
    auto [val, ul] = map.insert(10, "foo");
    EXPECT_FALSE(val);
  }

  // test removal
  EXPECT_EQ(map.size(), 2);
  map.remove(10);
  EXPECT_EQ(map.size(), 1);
  {
    auto [val, ul] = map.find(10);
    EXPECT_FALSE(val);
  }
}

TEST(SynchronizedMap, Parallel)
{
  constexpr int kThreads = 16;
  constexpr int kInsertions = 1024;

  SynchronizedMap<int, int> map;
  std::vector<std::jthread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; i++) {
    threads.emplace_back([&map] {
      for (int i = 0; i < kInsertions; i++) {
        auto [val, ul] = map[i];
        ++val;
      }
    });
  }
  threads.clear();

  EXPECT_EQ(map.size(), kInsertions);
  int tmp = 0;
  map.for_each([&tmp, kInsertions, kThreads](std::pair<const int, int> value) {
    EXPECT_LT(value.first, kInsertions);
    EXPECT_EQ(value.second, kThreads);
    tmp += value.second;
  });
  EXPECT_EQ(tmp, kInsertions * kThreads);

  std::atomic<int> atmp = 0;
  map.for_each_parallel(
      [&atmp, kInsertions, kThreads](std::pair<const int, int> value) {
        EXPECT_LT(value.first, kInsertions);
        EXPECT_EQ(value.second, kThreads);
        atmp += value.second;
      });
  EXPECT_EQ(atmp, kInsertions * kThreads);
}

using CoroMap =
    SynchronizedMap<int, int, std::hash<int>, std::equal_to<int>,
                    std::allocator<std::pair<const int, int>>, x23::CoroLock>;

TEST(SynchronizedMap, CoroContentionAndGuardReuse)
{
  auto run = []() -> x23::Async<void> {
    CoroMap map(1);
    x23::CoroGuard guard;
    auto *value = co_await map.insert(1, guard, 10);
    EXPECT_EQ(map.find(1, std::as_const(guard)), value);
    bool resumed = false;
    auto waiter = [&]() -> x23::Async<void> {
      x23::CoroGuard next;
      auto *found = co_await map.find(1, next);
      EXPECT_EQ(*found, 20);
      resumed = true;
    };
    auto pending = waiter();
    EXPECT_FALSE(resumed);
    *value = 20;
    guard.reset();
    co_await pending;
    EXPECT_TRUE(resumed);
    co_await map.for_each([](auto &entry) { ++entry.second; });
    EXPECT_EQ(co_await map.size(), 1);
    EXPECT_EQ(*(co_await map.find(1, guard)), 21);
    guard.reset();
    co_await map.clear();
    EXPECT_EQ(co_await map.size(), 0);
  };
  run()();
}

TEST(SynchronizedMap, CoroParallelDrainsAfterException)
{
  CoroMap map({{0, 0}, {1, 0}, {2, 0}}, 4);
  WorkerPool pool(1);
  std::binary_semaphore start(0);
  // Keep every chunk queued until the caller has started awaiting the jobs.
  pool.submit_detached([&] { start.acquire(); });
  std::atomic<int> visited = 0;
  auto pending = map.for_each_parallel(
      [&](auto &entry) {
        if (entry.first == 0)
          throw std::runtime_error("callback failure");
        ++visited;
      },
      1, pool);
  start.release();
  EXPECT_THROW(pending(), std::runtime_error);
  EXPECT_EQ(visited.load(), 2);
}

} // namespace hogd
