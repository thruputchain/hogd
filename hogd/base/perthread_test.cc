#include <gtest/gtest.h>

#include <latch>
#include <mutex>
#include <thread>
#include <vector>

#include "hogd/base/perthread.h"

namespace hogd {

static constexpr int kThreads = 16;

TEST(PerThreadTest, BasicUsage)
{
  PerThread<int> val;

  std::vector<std::jthread> joins;
  joins.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    joins.emplace_back([i, &val] {
      *val += i;
      EXPECT_EQ(*val, i);
    });
  }

  *val += kThreads;
  EXPECT_EQ(*val, kThreads);
}

TEST(PerThreadTest, ForEach)
{
  PerThread<std::vector<int>> vecs;

  std::latch vecs_done(kThreads);
  std::latch exit(1);
  std::vector<std::jthread> joins;
  joins.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    joins.emplace_back([&vecs, &vecs_done, &exit] {
      vecs->push_back(1);
      EXPECT_EQ(vecs->size(), 1U);
      vecs_done.count_down();
      exit.wait();
    });
  }

  std::vector<int> vec;
  vecs_done.wait();
  std::unique_lock ul = vecs.lock_threads();
  vecs.for_each(ul, [&vec](const std::vector<int> &v) {
    EXPECT_EQ(v.size(), 1U);
    vec.insert(vec.end(), v.cbegin(), v.cend());
  });
  constexpr static unsigned int kThreadsUnsigned = kThreads;
  EXPECT_EQ(vec.size(), kThreadsUnsigned);
  exit.count_down();
}

TEST(PerThreadTest, Counters)
{
  PerThreadCounter<int> cnt;

  // test threads that have exited.
  std::vector<std::jthread> joins;
  joins.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    joins.emplace_back([&cnt] { cnt.inc(); });
  }
  joins.clear();
  EXPECT_EQ(cnt.get(), kThreads);

  // test threads that are still active.
  std::jthread([&cnt] {
    cnt.inc();
    EXPECT_EQ(cnt.get(), kThreads + 1);
  }).join();
  cnt.inc();
  EXPECT_EQ(cnt.get(), kThreads + 2);
}

} // namespace hogd
