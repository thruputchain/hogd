#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <thread>
#include <unordered_set>
#include <vector>

#include "hogd/base/block_id.h"

namespace hogd {

TEST(BlockIdTest, Generate)
{
  uint64_t first = BlockIdGenerator::generate();
  EXPECT_NE(first, 0UL);

  uint64_t second = BlockIdGenerator::generate();
  EXPECT_NE(second, 0UL);
  EXPECT_NE(first, second);
}

TEST(BlockIdTest, SurvivesClockRollback)
{
  // A rolled-back or repeated clock reading must still yield a fresh ID.
  constexpr int64_t kFirst = 1'700'000'000'000'000'000;
  constexpr int64_t kLater = kFirst + 25;

  std::atomic<uint64_t> last = 0;

  const uint64_t first = BlockIdGenerator::next(last, kFirst);
  const uint64_t later = BlockIdGenerator::next(last, kLater);
  const uint64_t rolled_back = BlockIdGenerator::next(last, kFirst);
  const uint64_t stuck = BlockIdGenerator::next(last, kFirst);

  // A clock that advances is reported verbatim.
  EXPECT_EQ(first, static_cast<uint64_t>(kFirst));
  EXPECT_EQ(later, static_cast<uint64_t>(kLater));

  // A clock that steps back to an already-issued reading, and one that stays
  // there, both keep handing out fresh IDs.
  EXPECT_GT(rolled_back, later);
  EXPECT_GT(stuck, rolled_back);
}

TEST(BlockIdTest, SurvivesPreEpochClock)
{
  // Pre-epoch readings must clamp low, not wrap high.
  std::atomic<uint64_t> last = 0;

  const uint64_t first = BlockIdGenerator::next(last, -1);
  const uint64_t repeated = BlockIdGenerator::next(last, -1);
  const uint64_t earlier = BlockIdGenerator::next(last, -1'000'000'000);
  const uint64_t corrected =
      BlockIdGenerator::next(last, 1'700'000'000'000'000'000);

  // Pre-epoch readings land at the bottom of the range, not the top.
  EXPECT_LT(first, 1'000UL);

  // They still advance, and a corrected clock keeps going forward.
  EXPECT_GT(repeated, first);
  EXPECT_GT(earlier, repeated);
  EXPECT_GT(corrected, earlier);
}

TEST(BlockIdTest, UniqueUnderContention)
{
  // [generate] promises process-lifetime uniqueness across threads.
  constexpr size_t kThreads = 4;
  constexpr size_t kIdsPerThread = 2000;

  std::vector<std::vector<uint64_t>> per_thread(kThreads);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  std::latch start(kThreads);

  for (size_t i = 0; i < kThreads; i++) {
    threads.emplace_back([&, i]() {
      per_thread[i].reserve(kIdsPerThread);
      start.arrive_and_wait();
      for (size_t j = 0; j < kIdsPerThread; j++) {
        per_thread[i].push_back(BlockIdGenerator::generate());
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }

  std::unordered_set<uint64_t> seen;
  for (const std::vector<uint64_t> &ids : per_thread) {
    for (uint64_t id : ids) {
      EXPECT_TRUE(seen.insert(id).second) << "duplicate block ID " << id;
    }
  }
  EXPECT_EQ(seen.size(), kThreads * kIdsPerThread);
}

} // namespace hogd
