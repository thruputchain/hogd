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
  static constexpr uint128_t kInserting =
      ConcurrentSet<uint128_t>::Traits::kInserting;

  static size_t levels(const ConcurrentSet<uint128_t> &s) { return s.levels(); }

  // Add a second level without inserting any elements.
  static void publish_second_level(ConcurrentSet<uint128_t> &s)
  {
    s.inserts_.store(static_cast<size_t>(kInitialCapacity * kResizeLoadFactor),
                     std::memory_order_relaxed);
    s.try_resize();
    s.inserts_.store(0, std::memory_order_relaxed);
    s.erases_.store(0, std::memory_order_relaxed);
  }

  // Index of the first empty slot in value's probe sequence at level.
  static size_t first_probe_index(ConcurrentSet<uint128_t> &s, uint128_t value,
                                  size_t level)
  {
    const size_t hash = ConcurrentSet<uint128_t>::Traits::hash(value);
    const size_t capacity = ConcurrentSet<uint128_t>::entries_for_level(level);
    for (size_t i = 0; i < capacity; ++i) {
      const size_t index = (hash + i) & (capacity - 1);
      if (s.get_level(level)[index].load(std::memory_order_acquire) == kEmpty) {
        return index;
      }
    }
    ADD_FAILURE() << "no empty slot at level " << level;
    return 0;
  }

  static uint128_t slot_value(ConcurrentSet<uint128_t> &s, size_t level,
                              size_t index)
  {
    return s.get_level(level)[index].load(std::memory_order_acquire);
  }

  // Claim a slot the way try_insert does before publishing a value.
  static void claim_slot(ConcurrentSet<uint128_t> &s, size_t level,
                         size_t index)
  {
    uint128_t expected = kEmpty;
    ASSERT_TRUE(s.get_level(level)[index].compare_exchange_strong(
        expected, kInserting, std::memory_order_release,
        std::memory_order_relaxed));
    s.inserts_.fetch_add(1, std::memory_order_relaxed);
  }

  // Settle a claim as a tombstone, the way try_insert retires a claim it made
  // in a level that is no longer the deepest.
  static void settle_claim_as_tombstone(ConcurrentSet<uint128_t> &s,
                                        size_t level, size_t index)
  {
    ASSERT_EQ(kInserting, slot_value(s, level, index));
    s.get_level(level)[index].store(kTombstone, std::memory_order_release);
    s.erases_.fetch_add(1, std::memory_order_relaxed);
  }

  // Publish a value directly into a chosen level.
  static void publish_into_level(ConcurrentSet<uint128_t> &s, uint128_t value,
                                 size_t level, size_t index)
  {
    uint128_t expected = kEmpty;
    ASSERT_TRUE(s.get_level(level)[index].compare_exchange_strong(
        expected, value, std::memory_order_release, std::memory_order_relaxed));
    s.inserts_.fetch_add(1, std::memory_order_relaxed);
  }

  // Live copies of value across every level.
  static size_t count_value(ConcurrentSet<uint128_t> &s, uint128_t value)
  {
    size_t count = 0;
    for (size_t level = 0; level < s.levels(); ++level) {
      const size_t capacity =
          ConcurrentSet<uint128_t>::entries_for_level(level);
      for (size_t i = 0; i < capacity; ++i) {
        if (s.get_level(level)[i].load(std::memory_order_acquire) == value) {
          ++count;
        }
      }
    }
    return count;
  }

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
  ASSERT_EQ(InsertResult::kInserted, s.try_insert(1));
  ASSERT_EQ(s.size(), 1);

  ASSERT_EQ(InsertResult::kAlreadyPresent, s.try_insert(1));
  ASSERT_EQ(s.size(), 1);

  ASSERT_EQ(InsertResult::kInserted, s.try_insert(2));
  ASSERT_EQ(s.size(), 2);
  ASSERT_EQ(InsertResult::kAlreadyPresent, s.try_insert(2));
  ASSERT_EQ(s.size(), 2);
}

// The set reports a value it cannot store rather than trusting callers to
// pre-screen, so every marker it reserves has to be covered. A reserved value
// is also definitively absent, so lookups answer instead of failing.
TEST(ConcurrentSet, ReservedValuesAreRejectedNotStored)
{
  ConcurrentSet<uint128_t> s;
  for (uint128_t reserved :
       {ConcurrentSetHelper::kEmpty, ConcurrentSetHelper::kTombstone,
        ConcurrentSetHelper::kInserting}) {
    EXPECT_EQ(InsertResult::kValueReserved, s.try_insert(reserved));
    EXPECT_FALSE(s.contains(reserved));
    EXPECT_FALSE(s.erase(reserved));
  }
  EXPECT_EQ(0, s.size());

  // A value merely adjacent to a marker is ordinary and stores normally.
  EXPECT_EQ(InsertResult::kInserted,
            s.try_insert(ConcurrentSetHelper::kInserting - 1));
  EXPECT_TRUE(s.contains(ConcurrentSetHelper::kInserting - 1));
}

TEST(ConcurrentSet, Erase)
{
  ConcurrentSet<uint128_t> s;
  ASSERT_EQ(InsertResult::kInserted, s.try_insert(1));
  ASSERT_EQ(s.size(), 1);
  ASSERT_EQ(InsertResult::kAlreadyPresent, s.try_insert(1));
  ASSERT_EQ(s.size(), 1);
  ASSERT_TRUE(s.erase(1));
  ASSERT_EQ(s.size(), 0);
  ASSERT_EQ(InsertResult::kInserted, s.try_insert(1));
  ASSERT_EQ(s.size(), 1);
  ASSERT_TRUE(s.erase(1));
  ASSERT_EQ(InsertResult::kInserted, s.try_insert(1));
  ASSERT_EQ(InsertResult::kAlreadyPresent, s.try_insert(1));
  ASSERT_EQ(s.size(), 1);
}

TEST(ConcurrentSet, FullLevel0)
{
  ConcurrentSet<uint128_t> s;
  constexpr size_t kInsertsUntilResize = ConcurrentSetHelper::kInitialCapacity *
                                         ConcurrentSetHelper::kResizeLoadFactor;
  ASSERT_EQ(kInsertsUntilResize, 4);

  for (size_t i = 1; i < kInsertsUntilResize; ++i) {
    ASSERT_EQ(InsertResult::kInserted, s.try_insert(i));
    ASSERT_EQ(s.size(), i);
  }

  ASSERT_EQ(kInsertsUntilResize - 1,
            ConcurrentSetHelper::count_elements_at_level(s, 0));

  ASSERT_EQ(InsertResult::kInserted, s.try_insert(kInsertsUntilResize));

  std::set<uint128_t> expected_set;
  for (uint128_t i = 1; i <= kInsertsUntilResize; ++i)
    expected_set.insert(i);

  std::set<uint128_t> check_set;
  for (auto e : s)
    check_set.insert(e);

  ASSERT_EQ(expected_set, check_set);

  ASSERT_EQ(InsertResult::kInserted, s.try_insert(kInsertsUntilResize + 1));
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
    ASSERT_EQ(InsertResult::kInserted, s.try_insert(i + 1));
    ASSERT_EQ(s.size(), i + 1);
  }

  for (size_t i = 0; i < kInserts; ++i) {
    ASSERT_EQ(InsertResult::kAlreadyPresent, s.try_insert(i + 1));
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
    ASSERT_EQ(InsertResult::kInserted, s.try_insert(i + 1));
    ASSERT_EQ(s.size(), i + 1);
  }

  for (size_t i = 0; i < kInserts; ++i) {
    ASSERT_EQ(InsertResult::kAlreadyPresent, s.try_insert(i + 1));
  }

  for (size_t i = 0; i < kInserts; ++i) {
    ASSERT_TRUE(s.contains(i + 1));
  }
}

TEST(ConcurrentSet, GenericUint64)
{
  ConcurrentSet<uint64_t> s;
  ASSERT_EQ(InsertResult::kInserted, s.try_insert(1));
  ASSERT_EQ(s.size(), 1);
  ASSERT_TRUE(s.contains(1));
  ASSERT_TRUE(s.erase(1));
  ASSERT_FALSE(s.contains(1));
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
        ASSERT_EQ(InsertResult::kInserted,
                  s.try_insert((i * kKeysPerThread) + j));
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
        if (s.try_insert(j) == InsertResult::kInserted) {
          insert_counts[j - 1].fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  threads.clear();

  for (size_t j = 1; j <= kNKeys; ++j) {
    ASSERT_EQ(insert_counts[j - 1].load(), 1)
        << "key " << j << " was inserted " << insert_counts[j - 1].load()
        << " times";
  }

  for (size_t j = 1; j <= kNKeys; ++j) {
    ASSERT_TRUE(s.erase(j)) << "key " << j << " not found for erase";
    ASSERT_FALSE(s.contains(j))
        << "key " << j << " still present after erase (duplicate existed)";
  }
}

// try_insert claims a slot before publishing a value into it, so an erase that
// runs while an insert is still in flight cannot mistake that slot for a live
// element. Without the claim, an insert whose CAS straddled a resize would
// leave a copy of the value in a level that is no longer the deepest, and the
// erase below would consume its tombstone on that stale copy, report success,
// and leave the authoritative deeper copy live.
TEST(ConcurrentSet, EraseSkipsClaimedSlotAndRemovesLiveValue)
{
  ConcurrentSet<uint128_t> s;
  const uint128_t value = (uint128_t{10} << 64) | uint128_t{12345};

  ConcurrentSetHelper::publish_second_level(s);
  ASSERT_EQ(2, ConcurrentSetHelper::levels(s));

  // An insert that straddled the resize holds a claim in the older level, while
  // the value itself is published in the level that is now the deepest.
  const size_t claimed = ConcurrentSetHelper::first_probe_index(s, value, 0);
  ConcurrentSetHelper::claim_slot(s, 0, claimed);
  const size_t published = ConcurrentSetHelper::first_probe_index(s, value, 1);
  ConcurrentSetHelper::publish_into_level(s, value, 1, published);
  ASSERT_EQ(1, ConcurrentSetHelper::count_value(s, value));

  // The erase passes over the claim and removes the one live copy.
  ASSERT_TRUE(s.erase(value));
  EXPECT_EQ(0, ConcurrentSetHelper::count_value(s, value));
  EXPECT_FALSE(s.contains(value));
  EXPECT_EQ(ConcurrentSetHelper::kInserting,
            ConcurrentSetHelper::slot_value(s, 0, claimed));

  // The straddling insert then settles its claim as a tombstone, which is what
  // try_insert does when it finds a deeper level, leaving the set empty.
  ConcurrentSetHelper::settle_claim_as_tombstone(s, 0, claimed);
  EXPECT_EQ(0, s.size());
  EXPECT_EQ(InsertResult::kInserted, s.try_insert(value));
  EXPECT_TRUE(s.contains(value));
}

// An insert may not walk past a slot another insert has claimed, since that
// claim can settle into the same value and leave two live copies in one level.
// It cannot tell which value is coming and it must not wait, so it cancels the
// claim; the owner sees its publish fail and retries.
TEST(ConcurrentSet, InsertCancelsClaimItCannotWalkPast)
{
  ConcurrentSet<uint128_t> s;
  const uint128_t value = (uint128_t{7} << 64) | uint128_t{99};

  ConcurrentSetHelper::publish_second_level(s);
  const size_t claimed = ConcurrentSetHelper::first_probe_index(s, value, 1);
  ConcurrentSetHelper::claim_slot(s, 1, claimed);

  // The insert cancels the claim and publishes elsewhere in the same level.
  ASSERT_EQ(InsertResult::kInserted, s.try_insert(value));
  EXPECT_EQ(ConcurrentSetHelper::kTombstone,
            ConcurrentSetHelper::slot_value(s, 1, claimed));
  EXPECT_EQ(1, ConcurrentSetHelper::count_value(s, value));
  EXPECT_TRUE(s.contains(value));
  EXPECT_EQ(1, s.size());
}

// A try_insert that returned true owns its element until the same thread erases
// it. Two ways to break that are an erase that removes a transient copy instead
// of the live one, and an erase whose scan removes an element published after
// that scan started; both show up here as a lost element.
TEST(ConcurrentSet, ParallelInsertEraseSameKeysKeepsOwnership)
{
  constexpr size_t kNThreads = 16;
  constexpr size_t kNKeys = 8;
  constexpr size_t kNTrials = 500;

  std::atomic<size_t> stolen{0}; // a live element vanished under its owner
  std::atomic<size_t> erase_missed{0}; // an owner's erase found nothing
  size_t survivors = 0;                // an element outlived its own erase

  // A fresh set per trial keeps resizes frequent. inserts_ is monotonic and
  // capacity doubles, so inside one long-lived set the resize windows this
  // exercises become exponentially rarer.
  for (size_t trial = 0; trial < kNTrials; ++trial) {
    ConcurrentSet<uint128_t> s;
    std::vector<uint128_t> keys(kNKeys);
    for (size_t j = 0; j < kNKeys; ++j) {
      keys[j] = (uint128_t{trial + 1} << 64) | uint128_t{j + 1};
    }

    {
      std::barrier start(kNThreads);
      std::vector<std::jthread> threads;
      threads.reserve(kNThreads);
      for (size_t t = 0; t < kNThreads; ++t) {
        threads.emplace_back([&, t]() {
          start.arrive_and_wait();
          for (size_t j = 0; j < kNKeys; ++j) {
            const uint128_t key = keys[(t + j) % kNKeys];
            if (s.try_insert(key) != InsertResult::kInserted) {
              continue;
            }
            // Only the thread that won the insert erases the key, so nothing
            // else may remove this element before the erase below.
            if (!s.contains(key)) {
              stolen.fetch_add(1, std::memory_order_relaxed);
            }
            if (!s.erase(key)) {
              erase_missed.fetch_add(1, std::memory_order_relaxed);
            }
          }
        });
      }
    }

    // The threads have joined, so every won insert has been matched by an erase
    // and nothing may remain.
    for (const uint128_t &key : keys) {
      survivors += ConcurrentSetHelper::count_value(s, key);
    }
  }

  EXPECT_EQ(0, stolen.load()) << "a live element was removed by another thread";
  EXPECT_EQ(0, erase_missed.load()) << "an owner's erase found no element";
  EXPECT_EQ(0, survivors) << "an element outlived its own erase";
}

} // namespace hogd
