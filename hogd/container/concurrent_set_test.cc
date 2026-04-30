#include <barrier>
#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/container/concurrent_set.h"

namespace hogd {

class ConcurrentSetHelper {
public:
  static constexpr size_t kInitialCapacity =
      ConcurrentSet<uint128_t>::kInitialCapacity;
  static constexpr double kResizeLoadFactor =
      ConcurrentSet<uint128_t>::kResizeLoadFactor;
  static constexpr uint128_t kTombstone =
      ConcurrentSet<uint128_t>::Traits::kTombstone;
  static constexpr uint128_t kEmpty = ConcurrentSet<uint128_t>::Traits::kEmpty;

  static size_t levels(const ConcurrentSet<uint128_t> &s) { return s.levels(); }

  static size_t count_elements_at_level(ConcurrentSet<uint128_t> &s,
                                        size_t level)
  {
    size_t level_capacity = ConcurrentSet<uint128_t>::entries_for_level(level);
    size_t count = 0;
    for (size_t i = 0; i < level_capacity; i++) {
      if (s.get_level(level)[i].load(std::memory_order_relaxed) != kEmpty) {
        count++;
      }
    }
    return count;
  }
};

TEST(ConcurrentSet, Basic)
{
  ConcurrentSet<uint128_t> s;
  ASSERT_TRUE(s.try_insert(1));
  ASSERT_EQ(s.size(), 1);

  ASSERT_FALSE(s.try_insert(1));
  ASSERT_EQ(s.size(), 1);

  ASSERT_TRUE(s.try_insert(2));
  ASSERT_EQ(s.size(), 2);
  ASSERT_FALSE(s.try_insert(2));
  ASSERT_EQ(s.size(), 2);
}

TEST(ConcurrentSet, DISABLED_ZeroInsert)
{
  ConcurrentSet<uint128_t> s;
  ASSERT_TRUE(s.try_insert(ConcurrentSetHelper::kEmpty));
}

TEST(ConcurrentSet, DISABLED_DisallowTombstoneInsert)
{
  ConcurrentSet<uint128_t> s;
  ASSERT_FALSE(s.try_insert(ConcurrentSetHelper::kTombstone));
}

TEST(ConcurrentSet, Erase)
{
  ConcurrentSet<uint128_t> s;
  ASSERT_TRUE(s.try_insert(1));
  ASSERT_EQ(s.size(), 1);
  ASSERT_FALSE(s.try_insert(1));
  ASSERT_EQ(s.size(), 1);
  ASSERT_TRUE(s.erase(1));
  ASSERT_EQ(s.size(), 0);
  ASSERT_TRUE(s.try_insert(1));
  ASSERT_EQ(s.size(), 1);
  ASSERT_TRUE(s.erase(1));
  ASSERT_TRUE(s.try_insert(1));
  ASSERT_FALSE(s.try_insert(1));
  ASSERT_EQ(s.size(), 1);
}

TEST(ConcurrentSet, FullLevel0)
{
  ConcurrentSet<uint128_t> s;
  constexpr size_t kInsertsUntilResize = ConcurrentSetHelper::kInitialCapacity *
                                         ConcurrentSetHelper::kResizeLoadFactor;
  ASSERT_EQ(kInsertsUntilResize, 4);

  for (size_t i = 1; i < kInsertsUntilResize; ++i) {
    ASSERT_TRUE(s.try_insert(i));
    ASSERT_EQ(s.size(), i);
  }

  ASSERT_EQ(kInsertsUntilResize - 1,
            ConcurrentSetHelper::count_elements_at_level(s, 0));

  ASSERT_TRUE(s.try_insert(kInsertsUntilResize));

  std::set<uint128_t> expected_set;
  for (uint128_t i = 1; i <= kInsertsUntilResize; ++i)
    expected_set.insert(i);

  std::set<uint128_t> check_set;
  for (auto e : s)
    check_set.insert(e);

  ASSERT_EQ(expected_set, check_set);

  ASSERT_TRUE(s.try_insert(kInsertsUntilResize + 1));
  ASSERT_EQ(kInsertsUntilResize,
            ConcurrentSetHelper::count_elements_at_level(s, 0));
  ASSERT_EQ(1, ConcurrentSetHelper::count_elements_at_level(s, 1));

  auto it = s.begin();
  auto pit = s.begin();
  while (++it != s.end()) {
    pit = it;
  }
  // iteration is level-by-level. since most recent insert in the first in the
  // second level is should be iterated last.
  ASSERT_EQ(kInsertsUntilResize + 1, *pit);
}

TEST(ConcurrentSet, FillAFewLevels)
{
  ConcurrentSet<uint128_t> s;
  constexpr size_t kInserts = ConcurrentSetHelper::kInitialCapacity * 16;
  for (size_t i = 0; i < kInserts; ++i) {
    ASSERT_TRUE(s.try_insert(i + 1));
    ASSERT_EQ(s.size(), i + 1);
  }

  for (size_t i = 0; i < kInserts; ++i) {
    ASSERT_FALSE(s.try_insert(i + 1));
  }

  std::set<uint128_t> expected_set;
  for (uint128_t i = 1; i <= kInserts; ++i)
    expected_set.insert(i);

  std::set<uint128_t> check_set;
  for (auto e : s)
    check_set.insert(e);

  ASSERT_EQ(expected_set, check_set);

  constexpr size_t kExpectedLevel0 = ConcurrentSetHelper::kInitialCapacity *
                                     ConcurrentSetHelper::kResizeLoadFactor;
  ASSERT_EQ(kExpectedLevel0,
            ConcurrentSetHelper::count_elements_at_level(s, 0));

  constexpr size_t kExpectedLevel1 = kExpectedLevel0 * 2;
  ASSERT_EQ(kExpectedLevel1,
            ConcurrentSetHelper::count_elements_at_level(s, 1));

  constexpr size_t kExpectedLevels = 6;
  ASSERT_EQ(kExpectedLevels, ConcurrentSetHelper::levels(s));
}

TEST(ConcurrentSet, Contains)
{
  ConcurrentSet<uint128_t> s;
  constexpr size_t kInserts = ConcurrentSetHelper::kInitialCapacity * 16;
  for (size_t i = 0; i < kInserts; ++i) {
    ASSERT_TRUE(s.try_insert(i + 1));
    ASSERT_EQ(s.size(), i + 1);
  }

  for (size_t i = 0; i < kInserts; ++i) {
    ASSERT_FALSE(s.try_insert(i + 1));
  }

  for (size_t i = 0; i < kInserts; ++i) {
    ASSERT_TRUE(s.contains(i + 1));
  }
}

TEST(ConcurrentSet, GenericUint64)
{
  ConcurrentSet<uint64_t> s;
  ASSERT_TRUE(s.try_insert(1));
  ASSERT_EQ(s.size(), 1);
  ASSERT_TRUE(s.contains(1));
  ASSERT_TRUE(s.erase(1));
  ASSERT_FALSE(s.contains(1));
}

TEST(ConcurrentSet, DISABLED_DeathOnReservedValues)
{
  // This test is fine, but not super important and slow.
  ConcurrentSet<uint64_t> s;
  EXPECT_DEATH(s.try_insert(0), "reserved value");
  EXPECT_DEATH(s.try_insert(~uint64_t(0)), "reserved value");
  EXPECT_DEATH(s.contains(0), "reserved value");
  EXPECT_DEATH(s.erase(~uint64_t(0)), "reserved value");
}

TEST(ConcurrentSet, ParallelInsert)
{
  constexpr size_t kKeysPerThread = ConcurrentSetHelper::kInitialCapacity * 128;
  constexpr size_t kNThreads = 16;

  ConcurrentSet<uint64_t> s;
  std::vector<std::jthread> threads;
  threads.reserve(kNThreads);
  std::barrier barrier(kNThreads);
  for (size_t i = 0; i < kNThreads; ++i) {
    threads.emplace_back([i, &s, &barrier]() {
      barrier.arrive_and_wait();
      for (size_t j = 1; j <= kKeysPerThread; ++j)
        s.try_insert((i * kKeysPerThread) + j);
    });
  }

  threads.clear();

  std::set<__uint128_t> expected_keys{};
  for (size_t i = 0; i < kNThreads; ++i)
    for (size_t j = 1; j <= kKeysPerThread; ++j)
      expected_keys.insert((i * kKeysPerThread) + j);

  std::set<__uint128_t> found_keys{};
  for (auto e : s)
    found_keys.insert(e);
  ASSERT_EQ(expected_keys, found_keys);

  for (size_t i = 0; i < kNThreads; ++i) {
    threads.emplace_back([i, &s]() {
      for (size_t j = 1; j <= kKeysPerThread; ++j)
        ASSERT_TRUE(s.contains((i * kKeysPerThread) + j));
    });
  }
}

TEST(ConcurrentSet, ParallelInsertAndErase)
{
  constexpr size_t kKeysPerThread = ConcurrentSetHelper::kInitialCapacity * 128;
  constexpr size_t kNThreads = 16;

  ConcurrentSet<uint64_t> s;
  std::vector<std::jthread> threads;
  threads.reserve(kNThreads);
  std::barrier barrier(kNThreads);
  for (size_t i = 0; i < kNThreads; ++i) {
    threads.emplace_back([i, &s, &barrier]() {
      barrier.arrive_and_wait();
      for (size_t j = 1; j <= kKeysPerThread; ++j)
        s.try_insert((i * kKeysPerThread) + j);
    });
  }

  threads.clear();

  for (size_t i = 0; i < kNThreads; ++i) {
    threads.emplace_back([i, &s]() {
      for (size_t j = 1; j <= kKeysPerThread; ++j) {
        ASSERT_TRUE(s.erase((i * kKeysPerThread) + j));
        ASSERT_TRUE(s.try_insert((i * kKeysPerThread) + j));
      }
    });
  }

  threads.clear();

  ASSERT_EQ(s.size(), kKeysPerThread * kNThreads);

  for (size_t i = 0; i < kNThreads; ++i) {
    threads.emplace_back([i, &s]() {
      for (size_t j = 1; j <= kKeysPerThread; ++j) {
        ASSERT_TRUE(s.contains((i * kKeysPerThread) + j));
        ASSERT_TRUE(s.erase((i * kKeysPerThread) + j));
      }
    });
  }

  threads.clear();

  ASSERT_EQ(s.size(), 0);
}

TEST(ConcurrentSet, ParallelInsertSameKeys)
{
  constexpr size_t kNKeys = ConcurrentSetHelper::kInitialCapacity * 64;
  constexpr size_t kNThreads = 16;

  ConcurrentSet<uint64_t> s;
  std::vector<std::jthread> threads;
  threads.reserve(kNThreads);
  std::barrier barrier(kNThreads);

  std::vector<std::atomic<size_t>> insert_counts(kNKeys);

  for (size_t i = 0; i < kNThreads; ++i) {
    threads.emplace_back([&s, &barrier, &insert_counts]() {
      barrier.arrive_and_wait();
      for (size_t j = 1; j <= kNKeys; ++j) {
        if (s.try_insert(j)) {
          insert_counts[j - 1].fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  threads.clear();

  for (size_t j = 1; j <= kNKeys; ++j) {
    ASSERT_EQ(insert_counts[j - 1].load(), 1)
        << "key " << j << " was inserted "
        << insert_counts[j - 1].load() << " times";
  }

  for (size_t j = 1; j <= kNKeys; ++j) {
    ASSERT_TRUE(s.erase(j)) << "key " << j << " not found for erase";
    ASSERT_FALSE(s.contains(j))
        << "key " << j << " still present after erase (duplicate existed)";
  }
}

} // namespace hogd
