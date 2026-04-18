#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/replay_cache_lock.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

namespace {
constexpr StorageCostModel kTestCostModel =
    StorageCostModel(12000, 1, 1, 7, 1234);

Uniquifier
u(BlockHeight const &height, uint64_t const &nonce)
{
  return Uniquifier{.block_expiry_height = height, .nonce = nonce};
}

// Helper to create a dummy LockContext for testing
inline LockContext
make_dummy_context(BlockHeight block_height = 0)
{
  // These are never dereferenced in ReplayCacheLock after initial setup
  static HogDBDAO *dummy_dao = nullptr;
  static Address dummy_addr = make_address(0);
  static ItemKey dummy_key = make_replay_cache_item_key(dummy_addr);
  return LockContext(dummy_dao, block_height, dummy_key, &kTestCostModel);
}

} // namespace

// Helper class that is a friend of ReplayCacheLock
class ReplayCacheLockTestHelper {
public:
  // Set base data for a specific height
  static void set_base(ReplayCacheLock &lock, BlockHeight height,
                       const ReplayCacheData &data)
  {
    lock.bases_[height] =
        std::make_unique<ReplayCacheLock::ActiveHeight>(ReplayCacheData{data});

    // Initialize metadata
    std::int64_t total = 0;
    x23::Vec<ReplayCacheHeightCount> heights;
    for (const auto &pair : lock.bases_) {
      auto s = static_cast<std::int64_t>(pair.second->data.nonces.size());
      total += s;
      if (s > 0) {
        heights.push_back({.height = pair.first,
                           .nonce_count = static_cast<unsigned int>(s)});
      }
    }
    lock.meta_cell_.emplace(
        ReplayCacheMetadata{.size = total, .heights = std::move(heights)});
    lock.meta_.store(&lock.meta_cell_.value());
  }

  // Set empty base data for a range of heights to avoid DAO calls
  static void set_empty_bases(ReplayCacheLock &lock, BlockHeight start,
                              BlockHeight end)
  {
    for (BlockHeight h = start; h <= end; ++h) {
      if (!lock.bases_.contains(h)) {
        lock.bases_[h] = std::make_unique<ReplayCacheLock::ActiveHeight>();
        lock.bases_[h]->is_new = false;
      }
    }
    // Initialize metadata if not already set
    if (lock.meta_.load() == nullptr) {
      lock.meta_cell_.emplace(ReplayCacheMetadata{.size = 0, .heights = {}});
      lock.meta_.store(&lock.meta_cell_.value());
    }
  }

  static Uniquifier u_from_uint128(__uint128_t v)
  {
    return Uniquifier(v >> 64, v & 0xFFFFFFFFFFFFFFFF);
  }

  static HogDBStatus<ReplayCacheData> build_result(const ReplayCacheLock &lock,
                                                   const LockContext &ctx,
                                                   BlockHeight height)
  {
    std::vector<Uniquifier> all_additions;
    all_additions.reserve(lock.conflicts_.size());
    for (const auto &value : lock.conflicts_) {
      all_additions.push_back(u_from_uint128(value));
    }
    std::ranges::sort(all_additions);
    return lock.build_result(ctx, height, all_additions);
  }
};

class ReplayCacheLockTest : public testing::Test {
protected:
  ReplayCacheLockTest() = default;
  ~ReplayCacheLockTest() override = default;

  void SetUp() override
  {
    height_ = 9;
    base_data_ = ReplayCacheData{.nonces = {100}};
  }

  BlockHeight height_;
  ReplayCacheData base_data_;
};

TEST_F(ReplayCacheLockTest, DisallowDuplicateConsume)
{
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_empty_bases(lock, height_ - 1, height_ + 2);
  ReplayCacheLockTestHelper::set_base(lock, height_ + 1, base_data_);
  auto ctx = make_dummy_context(height_);

  // Create first item: conflicts with a nonce in base_data_
  // base_data_ has u(height_ + 1, 100) = u(10, 100)
  PointerItem<ReplayCacheIt> item1(u(height_ + 1, 100));
  auto r1 = lock.try_atomic_change_lock(nullptr, &item1, ctx);
  ASSERT_FALSE(r1.has_value());
  EXPECT_EQ(r1.error(), HogDBError(HogDBCode::kConflict));

  // Create second item: doesn't conflict, should succeed
  PointerItem<ReplayCacheIt> item2(u(height_ + 1, 101));
  auto r2 = lock.try_atomic_change_lock(nullptr, &item2, ctx);
  ASSERT_TRUE(r2.has_value());

  // Try to add the same uniquifier again: conflicts with previous addition
  PointerItem<ReplayCacheIt> item3(u(height_ + 1, 101));
  auto r3 = lock.try_atomic_change_lock(nullptr, &item3, ctx);
  ASSERT_FALSE(r3.has_value());
  EXPECT_EQ(r3.error(), HogDBError(HogDBCode::kConflict));
}

TEST_F(ReplayCacheLockTest, DisallowConsumeInThePast)
{
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_empty_bases(lock, height_ - 1, height_ + 2);
  ReplayCacheLockTestHelper::set_base(lock, height_ + 1, base_data_);
  auto ctx = make_dummy_context(height_);

  // expiry == current height is okay, so this should be allowed.
  PointerItem<ReplayCacheIt> item(u(height_, 100));
  auto r = lock.try_atomic_change_lock(nullptr, &item, ctx);
  ASSERT_TRUE(r.has_value());

  // expiry < current height is not okay, so this should fail.
  PointerItem<ReplayCacheIt> item2(u(height_ - 1, 100));
  auto r2 = lock.try_atomic_change_lock(nullptr, &item2, ctx);
  ASSERT_FALSE(r2.has_value());
  EXPECT_EQ(r2.error(), HogDBError(HogDBCode::kConflict));
}

TEST_F(ReplayCacheLockTest, MergeConflict)
{
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_empty_bases(lock, height_ - 1, height_ + 3);
  ReplayCacheLockTestHelper::set_base(lock, height_ + 1, base_data_);
  auto ctx = make_dummy_context(height_);

  // Tx1 adds 101
  PointerItem<ReplayCacheIt> item1(u(height_ + 1, 101));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, ctx).has_value());

  // Tx2 adds 102 (distinct) -> OK
  PointerItem<ReplayCacheIt> item2(u(height_ + 1, 102));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item2, ctx).has_value());

  // Tx3 adds 101 (same as Tx1) -> Conflict
  PointerItem<ReplayCacheIt> item3(u(height_ + 1, 101));
  auto r3 = lock.try_atomic_change_lock(nullptr, &item3, ctx);
  ASSERT_FALSE(r3.has_value());
  EXPECT_EQ(r3.error(), HogDBError(HogDBCode::kConflict));

  // Tx4 adds 101 but different height -> OK (New behavior: distinct uniquifier)
  PointerItem<ReplayCacheIt> item4(u(height_ + 2, 101));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item4, ctx).has_value());
}

TEST_F(ReplayCacheLockTest, UndoMerge)
{
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_empty_bases(lock, height_ - 1, height_ + 2);
  ReplayCacheLockTestHelper::set_base(lock, height_ + 1, base_data_);
  auto ctx = make_dummy_context(height_);

  // 1. Acquire 101
  PointerItem<ReplayCacheIt> item1(u(height_ + 1, 101));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, ctx).has_value());

  // 2. Try to acquire 101 again -> Conflict
  PointerItem<ReplayCacheIt> item2(u(height_ + 1, 101));
  auto r2 = lock.try_atomic_change_lock(nullptr, &item2, ctx);
  ASSERT_FALSE(r2.has_value());
  EXPECT_EQ(r2.error(), HogDBError(HogDBCode::kConflict));

  // 3. Release 101 (Undo Merge)
  ASSERT_TRUE(lock.try_atomic_change_lock(&item1, nullptr, ctx).has_value());

  // 4. Try to acquire 101 again -> Success
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item2, ctx).has_value());

  // 5. Complex case: Acquire 103 and 104
  PointerItem<ReplayCacheIt> item3(u(height_ + 1, 103));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item3, ctx).has_value());

  PointerItem<ReplayCacheIt> item4(u(height_ + 1, 104));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item4, ctx).has_value());

  // 6. Verify conflicts
  PointerItem<ReplayCacheIt> item3_dup(u(height_ + 1, 103));
  ASSERT_FALSE(
      lock.try_atomic_change_lock(nullptr, &item3_dup, ctx).has_value());

  PointerItem<ReplayCacheIt> item4_dup(u(height_ + 1, 104));
  ASSERT_FALSE(
      lock.try_atomic_change_lock(nullptr, &item4_dup, ctx).has_value());

  // 7. Release 103
  ASSERT_TRUE(lock.try_atomic_change_lock(&item3, nullptr, ctx).has_value());

  // 8. Verify 103 success, 104 still conflict
  ASSERT_TRUE(
      lock.try_atomic_change_lock(nullptr, &item3_dup, ctx).has_value());
  ASSERT_FALSE(
      lock.try_atomic_change_lock(nullptr, &item4_dup, ctx).has_value());
}

TEST_F(ReplayCacheLockTest, Expiry)
{
  {
    ReplayCacheLock lock;
    ReplayCacheLockTestHelper::set_empty_bases(lock, height_ - 1, height_ + 2);
    ReplayCacheLockTestHelper::set_base(lock, height_ + 1, base_data_);
    auto ctx = make_dummy_context(height_);

    // u(height_, 101) -> expiry 9. Current height 9. Allowed (>=).
    PointerItem<ReplayCacheIt> item1(u(height_, 101));
    ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, ctx).has_value());

    // u(height_ + 1, 100) -> expiry 10. Base has u(10, 100). Conflict.
    PointerItem<ReplayCacheIt> item2(u(height_ + 1, 100));
    ASSERT_FALSE(lock.try_atomic_change_lock(nullptr, &item2, ctx).has_value());

    // u(height_ + 1, 101) -> expiry 10. Allowed.
    PointerItem<ReplayCacheIt> item3(u(height_ + 1, 101));
    ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item3, ctx).has_value());
  }

  height_++; // height is now 10

  {
    ReplayCacheLock lock;
    ReplayCacheLockTestHelper::set_empty_bases(lock, height_ - 1, height_ + 2);
    ReplayCacheLockTestHelper::set_base(lock, height_, base_data_);
    auto ctx = make_dummy_context(height_);

    // u(height_, 101) -> expiry 10. Current height 10. Allowed.
    PointerItem<ReplayCacheIt> item1(u(height_, 101));
    ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, ctx).has_value());

    // u(height_, 100) -> expiry 10. Base has u(10, 100). Conflict.
    PointerItem<ReplayCacheIt> item2(u(height_, 100));
    ASSERT_FALSE(lock.try_atomic_change_lock(nullptr, &item2, ctx).has_value());

    // u(height_ + 1, 100) -> expiry 11. Allowed.
    PointerItem<ReplayCacheIt> item3(u(height_ + 1, 100));
    ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item3, ctx).has_value());

    // u(height_ + 1, 101) -> expiry 11. Allowed.
    PointerItem<ReplayCacheIt> item4(u(height_ + 1, 101));
    ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item4, ctx).has_value());

    // Build result at height 10 - should have base nonce (100) plus addition
    // (101)
    auto out_h10 = ReplayCacheLockTestHelper::build_result(lock, ctx, height_);
    ASSERT_TRUE(out_h10.has_value());
    ASSERT_EQ(2, out_h10->nonces.size());
    EXPECT_EQ(100, out_h10->nonces[0]); // u(10, 100) from base
    EXPECT_EQ(101, out_h10->nonces[1]); // u(10, 101) from additions

    // Build result at height 11 - should have additions only (100, 101)
    auto out_h11 =
        ReplayCacheLockTestHelper::build_result(lock, ctx, height_ + 1);
    ASSERT_TRUE(out_h11.has_value());
    ASSERT_EQ(2, out_h11->nonces.size());
    EXPECT_EQ(100, out_h11->nonces[0]); // u(11, 100) from additions
    EXPECT_EQ(101, out_h11->nonces[1]); // u(11, 101) from additions
  }
}

TEST_F(ReplayCacheLockTest, Build)
{
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_empty_bases(lock, height_ - 1, height_ + 2);
  ReplayCacheLockTestHelper::set_base(lock, height_ + 1, base_data_);
  auto ctx = make_dummy_context(height_);

  PointerItem<ReplayCacheIt> item1(u(height_, 102));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, ctx).has_value());

  PointerItem<ReplayCacheIt> item2(u(height_, 101));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item2, ctx).has_value());

  // Build result for height 9 - should have additions (101, 102) sorted
  auto out = ReplayCacheLockTestHelper::build_result(lock, ctx, height_);
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(2, out->nonces.size());
  EXPECT_EQ(101, out->nonces[0]); // sorted
  EXPECT_EQ(102, out->nonces[1]);

  // Build result for height 10 - should have base nonce (100)
  auto out_h10 =
      ReplayCacheLockTestHelper::build_result(lock, ctx, height_ + 1);
  ASSERT_TRUE(out_h10.has_value());
  ASSERT_EQ(1, out_h10->nonces.size());
  EXPECT_EQ(100, out_h10->nonces[0]); // from base

  // Test that remade lock with the built data detects conflicts
  ReplayCacheLock remade;
  ReplayCacheLockTestHelper::set_base(remade, height_, *out);
  ReplayCacheLockTestHelper::set_base(remade, height_ + 1, *out_h10);
  ASSERT_FALSE(remade.try_atomic_change_lock(nullptr, &item1, ctx).has_value());
  ASSERT_FALSE(remade.try_atomic_change_lock(nullptr, &item2, ctx).has_value());

  PointerItem<ReplayCacheIt> item3(u(height_ + 1, 100));
  ASSERT_FALSE(remade.try_atomic_change_lock(nullptr, &item3, ctx).has_value());
}

TEST_F(ReplayCacheLockTest, MergeCost)
{
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_empty_bases(lock, height_ - 1, height_ + 2);
  ReplayCacheLockTestHelper::set_base(lock, height_ + 1, base_data_);
  auto ctx = make_dummy_context(height_);

  { // merging non-conflicting entry results adds cost
    PointerItem<ReplayCacheIt> item(u(height_ + 1, 101));
    auto r = lock.try_atomic_change_lock(nullptr, &item, ctx);
    ASSERT_TRUE(r);
    EXPECT_EQ(kTestCostModel.get_replay_cache_cost(2), *r);
  }

  { // merging one more non-conflicting entry in requires
    // one more entry worth of cost to be locked
    PointerItem<ReplayCacheIt> item(u(height_ + 1, 102));
    auto r = lock.try_atomic_change_lock(nullptr, &item, ctx);
    ASSERT_TRUE(r);
    EXPECT_EQ(kTestCostModel.get_replay_cache_cost(2), *r);
  }

  // checks that merges and costs work with uniquifier expiry are in HogDBTest
  // since they are best tested with storage.
}

TEST_F(ReplayCacheLockTest, CommitChangeUsesCommittedAdditionsNotReservations)
{
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_empty_bases(lock, 7, 7);
  ReplayCacheLockTestHelper::set_base(lock, 4,
                                      ReplayCacheData{.nonces = {100}});
  ReplayCacheLockTestHelper::set_base(lock, 5,
                                      ReplayCacheData{.nonces = {200}});
  ReplayCacheLockTestHelper::set_base(lock, 6,
                                      ReplayCacheData{.nonces = {300}});

  auto ctx = make_dummy_context(7);
  const int128_t snapshot_cost = kTestCostModel.get_replay_cache_cost(3);

  PointerItem<ReplayCacheIt> item1(u(7, 400));
  auto r1 = lock.try_atomic_change_lock(nullptr, &item1, ctx);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(4), *r1);

  PointerItem<ReplayCacheIt> item2(u(7, 401));
  auto r2 = lock.try_atomic_change_lock(nullptr, &item2, ctx);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(4), *r2);

  int128_t commit_cost1 = lock.commit_change(true, snapshot_cost, item1, ctx);
  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(1) - snapshot_cost,
            commit_cost1);

  ASSERT_TRUE(lock.try_atomic_change_lock(&item2, nullptr, ctx).has_value());

  PointerItem<ReplayCacheIt> item3(u(7, 402));
  auto r3 = lock.try_atomic_change_lock(nullptr, &item3, ctx);
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(4), *r3);

  int128_t commit_cost2 = lock.commit_change(false, snapshot_cost, item3, ctx);
  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(2) -
                kTestCostModel.get_replay_cache_cost(1),
            commit_cost2);

  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(2) - snapshot_cost,
            commit_cost1 + commit_cost2);
}

TEST_F(ReplayCacheLockTest,
       CommitChangeTelescopesAcrossManyCommitsWithPartialPrune)
{
  // Seed heights 3 and 4 with one nonce each (both expire before ctx), plus
  // height 7 with one nonce (survives). Snapshot meta.size = 3; post-GC live
  // size at ctx.block_height == 5 is 1.
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_empty_bases(lock, 3, 7);
  ReplayCacheLockTestHelper::set_base(lock, 3,
                                      ReplayCacheData{.nonces = {100}});
  ReplayCacheLockTestHelper::set_base(lock, 4,
                                      ReplayCacheData{.nonces = {200}});
  ReplayCacheLockTestHelper::set_base(lock, 7,
                                      ReplayCacheData{.nonces = {300}});

  auto ctx = make_dummy_context(5);
  constexpr int kLiveAfterGc = 1;
  constexpr int kSnapshotSize = 3;
  constexpr int kNumCommits = 4;
  const int128_t snapshot_cost =
      kTestCostModel.get_replay_cache_cost(kSnapshotSize);

  std::vector<PointerItem<ReplayCacheIt>> items;
  items.reserve(kNumCommits);
  for (int i = 0; i < kNumCommits; ++i) {
    items.emplace_back(u(5, 400 + i));
    auto r = lock.try_atomic_change_lock(nullptr, &items.back(), ctx);
    ASSERT_TRUE(r.has_value());
    // Reservations are snapshot-based: every one returns cost(snapshot + 1).
    EXPECT_EQ(kTestCostModel.get_replay_cache_cost(kSnapshotSize + 1), *r);
  }

  int128_t total_commit_delta = 0;
  for (int i = 0; i < kNumCommits; ++i) {
    int128_t delta = lock.commit_change(true, snapshot_cost, items[i], ctx);
    total_commit_delta += delta;
    if (i == 0) {
      // First commit realizes the snapshot -> post-GC transition.
      EXPECT_EQ(kTestCostModel.get_replay_cache_cost(kLiveAfterGc + 1) -
                    snapshot_cost,
                delta);
    }
    else {
      // Subsequent commits charge incremental growth from the already-
      // committed size, not from the snapshot.
      EXPECT_EQ(kTestCostModel.get_replay_cache_cost(kLiveAfterGc + i + 1) -
                    kTestCostModel.get_replay_cache_cost(kLiveAfterGc + i),
                delta);
    }
  }

  // Closed form: the telescoping sum must equal cost(live + N) - snapshot.
  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(kLiveAfterGc + kNumCommits) -
                snapshot_cost,
            total_commit_delta);
}

TEST_F(ReplayCacheLockTest, CommitChangeIgnoresSnapshotCostPastFirstCommit)
{
  // Heights 3 and 4 both expire at ctx.block_height == 5, so live-after-GC = 0
  // and snapshot meta.size = 2.
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_empty_bases(lock, 3, 5);
  ReplayCacheLockTestHelper::set_base(lock, 3,
                                      ReplayCacheData{.nonces = {100}});
  ReplayCacheLockTestHelper::set_base(lock, 4,
                                      ReplayCacheData{.nonces = {200}});

  auto ctx = make_dummy_context(5);
  const int128_t snapshot_cost = kTestCostModel.get_replay_cache_cost(2);

  PointerItem<ReplayCacheIt> item1(u(5, 400));
  PointerItem<ReplayCacheIt> item2(u(5, 401));
  PointerItem<ReplayCacheIt> item3(u(5, 402));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, ctx).has_value());
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item2, ctx).has_value());
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item3, ctx).has_value());

  // First commit is the only one that reads snapshot_cost; pass the real
  // value here so the delta anchors the telescoping sum below.
  int128_t d1 = lock.commit_change(true, snapshot_cost, item1, ctx);
  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(1) - snapshot_cost, d1);

  // Commits 2 and 3 must ignore snapshot_cost entirely; pass obviously wrong
  // values (large negative, large positive) and also flip did_finalize both
  // ways. The returned deltas should match the pure telescoping formula.
  constexpr int128_t kGarbageNegative = -999'999'999;
  constexpr int128_t kGarbagePositive = int128_t{1} << 100;

  int128_t d2 = lock.commit_change(false, kGarbageNegative, item2, ctx);
  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(2) -
                kTestCostModel.get_replay_cache_cost(1),
            d2);

  int128_t d3 = lock.commit_change(true, kGarbagePositive, item3, ctx);
  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(3) -
                kTestCostModel.get_replay_cache_cost(2),
            d3);

  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(3) - snapshot_cost,
            d1 + d2 + d3);
}

TEST_F(ReplayCacheLockTest, CommitChangeNoPruneFirstCommitMatchesSubsequent)
{
  // Both existing heights are in the future relative to ctx, so nothing
  // expires. live-after-GC == meta.size, and the "snapshot -> post-GC"
  // transition must be a no-op: every commit should charge the same
  // incremental per-entry cost.
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_empty_bases(lock, 3, 6);
  ReplayCacheLockTestHelper::set_base(lock, 5,
                                      ReplayCacheData{.nonces = {100}});
  ReplayCacheLockTestHelper::set_base(lock, 6,
                                      ReplayCacheData{.nonces = {200}});

  auto ctx = make_dummy_context(3);
  const int128_t snapshot_cost = kTestCostModel.get_replay_cache_cost(2);

  PointerItem<ReplayCacheIt> item1(u(3, 400));
  PointerItem<ReplayCacheIt> item2(u(4, 401));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, ctx).has_value());
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item2, ctx).has_value());

  int128_t d1 = lock.commit_change(true, snapshot_cost, item1, ctx);
  int128_t d2 = lock.commit_change(true, snapshot_cost, item2, ctx);

  // No-prune invariant: commit 1 and commit 2 each charge exactly one entry.
  EXPECT_EQ(kTestCostModel.kReplayCacheCostPerEntry, d1);
  EXPECT_EQ(kTestCostModel.kReplayCacheCostPerEntry, d2);

  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(4) - snapshot_cost, d1 + d2);
}

TEST_F(ReplayCacheLockTest, CommitChangeAllPruneStartsFromEmptyLiveSize)
{
  // Every seeded height expires at ctx.block_height == 5, so live-after-GC
  // must be zero. The first committed addition transitions from snapshot
  // directly to cost(1); subsequent commits grow cost(1) -> cost(2) -> ...
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_empty_bases(lock, 3, 5);
  ReplayCacheLockTestHelper::set_base(lock, 3,
                                      ReplayCacheData{.nonces = {100, 101}});
  ReplayCacheLockTestHelper::set_base(lock, 4,
                                      ReplayCacheData{.nonces = {200}});

  auto ctx = make_dummy_context(5);
  const int128_t snapshot_cost = kTestCostModel.get_replay_cache_cost(3);

  PointerItem<ReplayCacheIt> item1(u(5, 400));
  PointerItem<ReplayCacheIt> item2(u(5, 401));
  PointerItem<ReplayCacheIt> item3(u(5, 402));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, ctx).has_value());
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item2, ctx).has_value());
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item3, ctx).has_value());

  int128_t d1 = lock.commit_change(true, snapshot_cost, item1, ctx);
  int128_t d2 = lock.commit_change(true, snapshot_cost, item2, ctx);
  int128_t d3 = lock.commit_change(true, snapshot_cost, item3, ctx);

  // First commit: live=0, new=cost(1). Must not underflow to cost(0).
  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(1) - snapshot_cost, d1);
  // Subsequent commits grow from the already-committed size.
  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(2) -
                kTestCostModel.get_replay_cache_cost(1),
            d2);
  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(3) -
                kTestCostModel.get_replay_cache_cost(2),
            d3);

  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(3) - snapshot_cost,
            d1 + d2 + d3);
}

TEST_F(ReplayCacheLockTest, ExpiryHeightEqualToCurrentIsKeptAndConsumable)
{
  // ctx.block_height == 5. Seeded heights are 4 (pruned) and 5 (kept: the GC
  // threshold uses lower_bound, so height == ctx is still live). Also verify
  // that a reservation at expiry == ctx is accepted.
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_empty_bases(lock, 4, 6);
  ReplayCacheLockTestHelper::set_base(lock, 4,
                                      ReplayCacheData{.nonces = {100}});
  ReplayCacheLockTestHelper::set_base(lock, 5,
                                      ReplayCacheData{.nonces = {200}});

  auto ctx = make_dummy_context(5);
  const int128_t snapshot_cost = kTestCostModel.get_replay_cache_cost(2);

  // Reservation at expiry == ctx.block_height is allowed.
  PointerItem<ReplayCacheIt> item1(u(5, 201));
  auto r1 = lock.try_atomic_change_lock(nullptr, &item1, ctx);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(3), *r1);

  PointerItem<ReplayCacheIt> item2(u(5, 202));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item2, ctx).has_value());

  // live-after-GC must be 1 (height 4 pruned; height 5 kept). The first
  // commit therefore goes snapshot(cost 2) -> cost(live+1)=cost(2), a zero
  // delta. If height==ctx were incorrectly pruned, live would be 0 and d1
  // would be cost(1)-cost(2)=-kReplayCacheCostPerEntry.
  int128_t d1 = lock.commit_change(true, snapshot_cost, item1, ctx);
  EXPECT_EQ(0, d1);

  int128_t d2 = lock.commit_change(true, snapshot_cost, item2, ctx);
  EXPECT_EQ(kTestCostModel.kReplayCacheCostPerEntry, d2);

  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(3) - snapshot_cost,
            d1 + d2);
}

TEST_F(ReplayCacheLockTest, RemovedReservationDoesNotAdvanceCommittedCounter)
{
  // All-prune setup (heights 3 and 4 both expire at ctx=5), so if the first
  // commit is accounted correctly the delta is cost(1) - snapshot = negative.
  // If a reservation-then-removal ever erroneously bumped
  // committed_additions_, the follow-up commit would take the second-commit
  // branch and return cost(1) - cost(0) = positive, which is a loud failure.
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_empty_bases(lock, 3, 5);
  ReplayCacheLockTestHelper::set_base(lock, 3,
                                      ReplayCacheData{.nonces = {100}});
  ReplayCacheLockTestHelper::set_base(lock, 4,
                                      ReplayCacheData{.nonces = {200}});

  auto ctx = make_dummy_context(5);
  const int128_t snapshot_cost = kTestCostModel.get_replay_cache_cost(2);

  // Reserve, then remove.
  PointerItem<ReplayCacheIt> dropped(u(5, 400));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &dropped, ctx).has_value());
  ASSERT_TRUE(lock.try_atomic_change_lock(&dropped, nullptr, ctx).has_value());

  // Re-reserve the same nonce; removal should have erased it from conflicts_.
  PointerItem<ReplayCacheIt> dropped_again(u(5, 400));
  ASSERT_TRUE(
      lock.try_atomic_change_lock(nullptr, &dropped_again, ctx).has_value());

  PointerItem<ReplayCacheIt> kept(u(5, 401));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &kept, ctx).has_value());

  // First real commit must take the first-commit branch: prev = snapshot,
  // new = cost(live+1) = cost(1).
  int128_t d1 = lock.commit_change(true, snapshot_cost, dropped_again, ctx);
  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(1) - snapshot_cost, d1);

  // Second commit: already-committed size = 1, new = cost(2).
  int128_t d2 = lock.commit_change(true, snapshot_cost, kept, ctx);
  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(2) -
                kTestCostModel.get_replay_cache_cost(1),
            d2);

  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(2) - snapshot_cost, d1 + d2);
}

} // namespace hogd
