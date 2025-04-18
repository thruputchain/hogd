#include <gtest/gtest.h>

#include <atomic>
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

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
