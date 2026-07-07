#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "hogd/base/io.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/replay_cache_lock.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db.h"
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

  EXPECT_EQ(kTestCostModel.get_replay_cache_cost(3) - snapshot_cost, d1 + d2);
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

TEST(ReplayCacheLockHeightCap, RejectsNewHeightWhenMetadataAlreadyAtCap)
{
  // Set up a real HogDBDAO over a temp RAMDB so get_base() can actually reach
  // its DAO-miss branch (where the height cap check lives).
  auto dir = Directory::open_temporary();
  ASSERT_TRUE(static_cast<bool>(dir));

  constexpr size_t kHeapSize = 10_MiB;
  auto db = Database::open(dir->get_path(),
                           Database::estimate_heap_size(kHeapSize), 0);
  ASSERT_TRUE(db);
  HogDBDAO dao(RAMDB(std::move(*db)));

  // Seed kReplayCacheHeightsMaxLen active heights (each with one nonce) for a
  // test address: both the metadata record AND the per-height data records
  // must exist, because get_base() consults both.
  Address addr = make_address(0xabc123);
  constexpr BlockHeight kSeedBase = 100;

  ReplayCacheMetadata meta;
  meta.size = kReplayCacheHeightsMaxLen;
  meta.heights.reserve(kReplayCacheHeightsMaxLen);
  for (std::int64_t i = 0; i < kReplayCacheHeightsMaxLen; ++i) {
    BlockHeight h = kSeedBase + static_cast<BlockHeight>(i);
    meta.heights.push_back(
        ReplayCacheHeightCount{.height = h, .nonce_count = 1});

    ReplayCacheData data;
    data.nonces.push_back(static_cast<int64_t>(h));
    ASSERT_TRUE(dao.write_address_replay_cache_data(addr, h, data));
  }
  ASSERT_TRUE(dao.write_address_replay_cache_meta(addr, meta));

  // Build a lock and a context pointing at the seeded DAO.
  ReplayCacheLock lock;
  ItemKey rckey = make_replay_cache_item_key(addr);
  BlockHeight current_height = 50;
  LockContext ctx(&dao, current_height, rckey, &kTestCostModel);

  // Attempt a consume_uniquifier at a NEW height that is strictly greater
  // than every seeded height (so it will miss in bases_ and in the DAO).
  BlockHeight new_height =
      kSeedBase + static_cast<BlockHeight>(kReplayCacheHeightsMaxLen) + 1000;
  PointerItem<ReplayCacheIt> item(u(new_height, 42));

  auto r = lock.try_atomic_change_lock(nullptr, &item, ctx);

  // Expected post-fix: the cap is at kReplayCacheHeightsMaxLen, so admitting
  // a brand new height here would make the post-commit total exceed the cap.
  // The call must return kConflict.
  ASSERT_FALSE(r.has_value())
      << "height cap must reject the 4001st height; got success instead";
  EXPECT_EQ(HogDBError(HogDBCode::kConflict), r.error());
}

// Regression for #2581: the height cap counts only *unexpired* heights. An
// address whose snapshot is full of expired heights (all below the current
// block) must still admit a fresh height, because those expired rows are
// pruned at block close. Counting them against the cap would create a
// lockout: no fresh change can enter, so the expired rows are never GC'd.
TEST(ReplayCacheLockHeightCap, AdmitsNewHeightWhenMetadataIsAllExpiredAtCap)
{
  auto dir = Directory::open_temporary();
  ASSERT_TRUE(static_cast<bool>(dir));

  constexpr size_t kHeapSize = 10_MiB;
  auto db = Database::open(dir->get_path(),
                           Database::estimate_heap_size(kHeapSize), 0);
  ASSERT_TRUE(db);
  HogDBDAO dao(RAMDB(std::move(*db)));

  // Seed exactly kReplayCacheHeightsMaxLen heights, all strictly below the
  // current block height, so every one of them is expired.
  Address addr = make_address(0xabc123);
  constexpr BlockHeight kSeedBase = 100;

  ReplayCacheMetadata meta;
  meta.size = kReplayCacheHeightsMaxLen;
  meta.heights.reserve(kReplayCacheHeightsMaxLen);
  for (std::int64_t i = 0; i < kReplayCacheHeightsMaxLen; ++i) {
    BlockHeight h = kSeedBase + static_cast<BlockHeight>(i);
    meta.heights.push_back(
        ReplayCacheHeightCount{.height = h, .nonce_count = 1});

    ReplayCacheData data;
    data.nonces.push_back(static_cast<int64_t>(h));
    ASSERT_TRUE(dao.write_address_replay_cache_data(addr, h, data));
  }
  ASSERT_TRUE(dao.write_address_replay_cache_meta(addr, meta));

  ReplayCacheLock lock;
  ItemKey rckey = make_replay_cache_item_key(addr);
  // Current height sits above every seeded height: all are expired.
  BlockHeight current_height =
      kSeedBase + static_cast<BlockHeight>(kReplayCacheHeightsMaxLen) + 1;
  LockContext ctx(&dao, current_height, rckey, &kTestCostModel);

  // A fresh, unexpired height (>= current_height). Pre-fix this is rejected
  // because the 4000 expired snapshot heights fill the budget.
  BlockHeight new_height = current_height + 1000;
  PointerItem<ReplayCacheIt> item(u(new_height, 42));

  auto r = lock.try_atomic_change_lock(nullptr, &item, ctx);

  ASSERT_TRUE(r.has_value())
      << "expired snapshot heights must not consume the active-height budget; "
         "the fresh height was wrongly rejected (#2581)";
}

TEST_F(ReplayCacheLockTest, MaxExpiryHeightBuildResult)
{
  // Regression for issue #1447: build_result must not compute
  // block_height + 1 when block_height == INT64_MAX (signed overflow / UB).
  constexpr BlockHeight kMaxHeight = std::numeric_limits<BlockHeight>::max();

  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_base(lock, kMaxHeight,
                                      ReplayCacheData{.nonces = {100}});
  auto ctx = make_dummy_context(0);

  PointerItem<ReplayCacheIt> item(u(kMaxHeight, 42));
  auto admit = lock.try_atomic_change_lock(nullptr, &item, ctx);
  ASSERT_TRUE(admit.has_value()) << "admission must accept expiry == INT64_MAX";

  auto out = ReplayCacheLockTestHelper::build_result(lock, ctx, kMaxHeight);
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(2U, out->nonces.size());
  EXPECT_EQ(42U, out->nonces[0]);
  EXPECT_EQ(100U, out->nonces[1]);
}

// ReplayCacheData::nonces must be unsigned — Uniquifier::nonce is uint64_t
// and binary_search / set_union rely on matching sort semantics.
static_assert(
    std::is_unsigned_v<decltype(ReplayCacheData{}.nonces)::value_type>,
    "ReplayCacheData::nonces must be unsigned to match Uniquifier::nonce");

TEST_F(ReplayCacheLockTest, HighBitNonces)
{
  // Verify that nonces spanning the full uint64 range are correctly handled
  // by binary_search (duplicate detection) and set_union (merge).
  constexpr uint64_t kSmall = 10;
  constexpr uint64_t kMidHigh = (uint64_t{1} << 63) + 50;
  constexpr uint64_t kHigh = UINT64_MAX - 5;

  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_empty_bases(lock, height_ - 1, height_ + 2);
  ReplayCacheData base;
  base.nonces = {kSmall, kMidHigh, kHigh};
  ReplayCacheLockTestHelper::set_base(lock, height_ + 1, base);
  auto ctx = make_dummy_context(height_);

  // Each base nonce must be detected as a duplicate.
  for (uint64_t nonce : {kSmall, kMidHigh, kHigh}) {
    PointerItem<ReplayCacheIt> item(u(height_ + 1, nonce));
    auto r = lock.try_atomic_change_lock(nullptr, &item, ctx);
    ASSERT_FALSE(r.has_value()) << "nonce " << nonce << " should conflict";
    EXPECT_EQ(r.error(), HogDBError(HogDBCode::kConflict));
  }

  // New nonces in both halves of the uint64 range must be accepted.
  constexpr uint64_t kNewLow = 20;
  constexpr uint64_t kNewHigh = (uint64_t{1} << 63) + 100;
  PointerItem<ReplayCacheIt> add_low(u(height_ + 1, kNewLow));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &add_low, ctx).has_value());
  PointerItem<ReplayCacheIt> add_high(u(height_ + 1, kNewHigh));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &add_high, ctx).has_value());

  // build_result must produce a correctly sorted merge.
  auto out = ReplayCacheLockTestHelper::build_result(lock, ctx, height_ + 1);
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(5U, out->nonces.size());
  EXPECT_EQ(kSmall, out->nonces[0]);
  EXPECT_EQ(kNewLow, out->nonces[1]);
  EXPECT_EQ(kMidHigh, out->nonces[2]);
  EXPECT_EQ(kNewHigh, out->nonces[3]);
  EXPECT_EQ(kHigh, out->nonces[4]);
}

TEST_F(ReplayCacheLockTest, BuildResultRangeSelectionFiltersByHeight)
{
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_empty_bases(lock, height_ - 1, height_ + 3);
  auto ctx = make_dummy_context(height_);

  PointerItem<ReplayCacheIt> item_lo(u(height_, 10));
  PointerItem<ReplayCacheIt> item_mid(u(height_ + 1, 20));
  PointerItem<ReplayCacheIt> item_hi(u(height_ + 2, 30));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item_lo, ctx).has_value());
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item_mid, ctx).has_value());
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item_hi, ctx).has_value());

  auto out = ReplayCacheLockTestHelper::build_result(lock, ctx, height_ + 1);
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(1U, out->nonces.size());
  EXPECT_EQ(20U, out->nonces[0]);
}

// #2663: the conflict-set hash must mix the high (expiry) limb, not just the
// low (nonce) limb the way std::hash<uint128_t> does on libstdc++. Same-nonce
// keys that differ only in expiry must land in different buckets so an
// attacker cannot pack the probe table.
TEST(ReplayConflictSetTraitsTest, HashMixesExpiryLimb)
{
  constexpr uint64_t kNonce = 0xDEADBEEFCAFEBABEULL;
  auto packed = [](uint64_t expiry, uint64_t n) {
    return (static_cast<uint128_t>(expiry) << 64) | n;
  };

  auto h50 = ReplayConflictSetTraits::hash(packed(50, kNonce));
  auto h51 = ReplayConflictSetTraits::hash(packed(51, kNonce));
  auto h52 = ReplayConflictSetTraits::hash(packed(52, kNonce));
  EXPECT_NE(h50, h51);
  EXPECT_NE(h51, h52);
  EXPECT_NE(h50, h52);

  // Stable within a process (same key hashes the same).
  EXPECT_EQ(h50, ReplayConflictSetTraits::hash(packed(50, kNonce)));
}

} // namespace hogd
