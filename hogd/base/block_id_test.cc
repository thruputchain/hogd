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
  uint64_t first = BlockIdGenerator::generate(BlockIdLane::kMempool);
  EXPECT_NE(first, 0UL);

  uint64_t second = BlockIdGenerator::generate(BlockIdLane::kMempool);
  EXPECT_NE(second, 0UL);
  EXPECT_NE(first, second);
}

TEST(BlockIdTest, SurvivesClockRollback)
{
  // A rolled-back or repeated clock reading must still yield a fresh ID.
  // Both readings are already in the lane, so they pass through verbatim.
  constexpr int64_t kFirst = 1'700'000'000'000'000'000;
  constexpr int64_t kLater = kFirst + 24;

  std::atomic<uint64_t> last = 0;
  const BlockIdLane kLane = BlockIdLane::kMempool;

  const uint64_t first = BlockIdGenerator::next(last, kLane, kFirst);
  const uint64_t later = BlockIdGenerator::next(last, kLane, kLater);
  const uint64_t rolled_back = BlockIdGenerator::next(last, kLane, kFirst);
  const uint64_t stuck = BlockIdGenerator::next(last, kLane, kFirst);

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
  const BlockIdLane kLane = BlockIdLane::kMempool;

  const uint64_t first = BlockIdGenerator::next(last, kLane, -1);
  const uint64_t repeated = BlockIdGenerator::next(last, kLane, -1);
  const uint64_t earlier = BlockIdGenerator::next(last, kLane, -1'000'000'000);
  const uint64_t corrected =
      BlockIdGenerator::next(last, kLane, 1'700'000'000'000'000'000);

  EXPECT_LT(first, 1'000UL);
  EXPECT_GT(repeated, first);
  EXPECT_GT(earlier, repeated);
  EXPECT_GT(corrected, earlier);
}

TEST(BlockIdTest, ReadingSnapsIntoLane)
{
  // A reading outside the lane floors to the largest one inside it, so an ID
  // never lands in another generator's residue class.
  constexpr int64_t kOdd = 1'700'000'000'000'000'001;

  std::atomic<uint64_t> mempool_last = 0;
  std::atomic<uint64_t> view_last = 0;

  EXPECT_EQ(BlockIdGenerator::next(mempool_last, BlockIdLane::kMempool, kOdd),
            static_cast<uint64_t>(kOdd) - 1);
  EXPECT_EQ(BlockIdGenerator::next(view_last, BlockIdLane::kViewSnapshot, kOdd),
            static_cast<uint64_t>(kOdd));
}

TEST(BlockIdTest, LanesStayDisjointAcrossProcesses)
{
  // The mempool and hog_db run as separate processes with separate counters.
  // Feeding both the same clock readings must still produce disjoint IDs.
  constexpr int64_t kStart = 1'700'000'000'000'000'000;

  std::atomic<uint64_t> mempool_last = 0;
  std::atomic<uint64_t> view_last = 0;
  std::unordered_set<uint64_t> seen;

  for (int64_t offset = 0; offset < 200; offset++) {
    const int64_t now_ns = kStart + offset;
    const uint64_t mempool_id =
        BlockIdGenerator::next(mempool_last, BlockIdLane::kMempool, now_ns);
    const uint64_t view_id =
        BlockIdGenerator::next(view_last, BlockIdLane::kViewSnapshot, now_ns);

    EXPECT_EQ(mempool_id % kBlockIdLaneCount,
              static_cast<uint64_t>(BlockIdLane::kMempool));
    EXPECT_EQ(view_id % kBlockIdLaneCount,
              static_cast<uint64_t>(BlockIdLane::kViewSnapshot));

    EXPECT_TRUE(seen.insert(mempool_id).second) << "reused " << mempool_id;
    EXPECT_TRUE(seen.insert(view_id).second) << "reused " << view_id;
  }
}

TEST(BlockIdTest, LanesStayDisjointWithinOneProcess)
{
  // hog-devnode hosts both servers, so one counter feeds both lanes. Each
  // base is handed out once, so the IDs stay distinct and stay in lane.
  constexpr int64_t kStart = 1'700'000'000'000'000'000;

  std::atomic<uint64_t> last = 0;
  std::unordered_set<uint64_t> seen;

  for (int64_t offset = 0; offset < 200; offset++) {
    const int64_t now_ns = kStart + offset;
    const uint64_t mempool_id =
        BlockIdGenerator::next(last, BlockIdLane::kMempool, now_ns);
    const uint64_t view_id =
        BlockIdGenerator::next(last, BlockIdLane::kViewSnapshot, now_ns);

    EXPECT_EQ(mempool_id % kBlockIdLaneCount,
              static_cast<uint64_t>(BlockIdLane::kMempool));
    EXPECT_EQ(view_id % kBlockIdLaneCount,
              static_cast<uint64_t>(BlockIdLane::kViewSnapshot));

    EXPECT_TRUE(seen.insert(mempool_id).second) << "reused " << mempool_id;
    EXPECT_TRUE(seen.insert(view_id).second) << "reused " << view_id;
  }
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
        per_thread[i].push_back(
            BlockIdGenerator::generate(BlockIdLane::kMempool));
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
      EXPECT_EQ(id % kBlockIdLaneCount,
                static_cast<uint64_t>(BlockIdLane::kMempool));
    }
  }
  EXPECT_EQ(seen.size(), kThreads * kIdsPerThread);
}

} // namespace hogd
