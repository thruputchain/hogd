#include "hogd/hogdb/item_lock/replay_cache_lock.h"

#include <gtest/gtest.h>

#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

namespace {
constexpr StorageCostModel kTestCostModel = StorageCostModel(12000, 1, 1, 1, 7);

Uniquifier
u(BlockHeight const &height, ReplayNonce const &nonce)
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
  return LockContext(*dummy_dao, block_height, dummy_key);
}

} // namespace

// Helper class that is a friend of ReplayCacheLock
class ReplayCacheLockTestHelper {
public:
  static void set_base(ReplayCacheLock &lock, const ReplayCacheData &data)
  {
    lock.base_replay_cache_ = data;
  }

  static ReplayCacheData build_result(const ReplayCacheLock &lock,
                                      BlockHeight height)
  {
    return lock.build_result(height);
  }
};

class ReplayCacheLockTest : public testing::Test {
protected:
  ReplayCacheLockTest() = default;
  ~ReplayCacheLockTest() override = default;

  void SetUp() override
  {
    height_ = 9;
    base_data_ = ReplayCacheData{.uniquifiers = {
                                     u(height_ + 1, 100),
                                 }};
  }

  BlockHeight height_;
  ReplayCacheData base_data_;
};

TEST_F(ReplayCacheLockTest, DisallowDuplicateConsume)
{
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_base(lock, base_data_);
  auto ctx = make_dummy_context(height_);

  // Create first item: conflicts with a nonce in base_data_
  // base_data_ has u(height_ + 1, 100) = u(10, 100)
  PointerItem<ReplayCacheIt> item1(u(height_ + 1, 100));
  auto r1 = lock.try_atomic_change_lock(nullptr, &item1, &kTestCostModel, ctx);
  ASSERT_FALSE(r1.has_value());
  EXPECT_EQ(r1.error(), HogDBError(HogDBCode::kConflict));

  // Create second item: doesn't conflict, should succeed
  PointerItem<ReplayCacheIt> item2(u(height_ + 1, 101));
  auto r2 = lock.try_atomic_change_lock(nullptr, &item2, &kTestCostModel, ctx);
  ASSERT_TRUE(r2.has_value());

  // Try to add the same uniquifier again: conflicts with previous addition
  PointerItem<ReplayCacheIt> item3(u(height_ + 1, 101));
  auto r3 = lock.try_atomic_change_lock(nullptr, &item3, &kTestCostModel, ctx);
  ASSERT_FALSE(r3.has_value());
  EXPECT_EQ(r3.error(), HogDBError(HogDBCode::kConflict));
}

TEST_F(ReplayCacheLockTest, DisallowConsumeInThePast)
{
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_base(lock, base_data_);
  auto ctx = make_dummy_context(height_);

  // expiry == current height is okay, so this should be allowed.
  PointerItem<ReplayCacheIt> item(u(height_, 100));
  auto r = lock.try_atomic_change_lock(nullptr, &item, &kTestCostModel, ctx);
  ASSERT_TRUE(r.has_value());

  // expiry < current height is not okay, so this should fail.
  PointerItem<ReplayCacheIt> item2(u(height_ - 1, 100));
  auto r2 = lock.try_atomic_change_lock(nullptr, &item2, &kTestCostModel, ctx);
  ASSERT_FALSE(r2.has_value());
  EXPECT_EQ(r2.error(), HogDBError(HogDBCode::kConflict));
}

TEST_F(ReplayCacheLockTest, MergeConflict)
{
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_base(lock, base_data_);
  auto ctx = make_dummy_context(height_);

  // Tx1 adds 101
  PointerItem<ReplayCacheIt> item1(u(height_ + 1, 101));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, &kTestCostModel, ctx)
                  .has_value());

  // Tx2 adds 102 (distinct) -> OK
  PointerItem<ReplayCacheIt> item2(u(height_ + 1, 102));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item2, &kTestCostModel, ctx)
                  .has_value());

  // Tx3 adds 101 (same as Tx1) -> Conflict
  PointerItem<ReplayCacheIt> item3(u(height_ + 1, 101));
  auto r3 = lock.try_atomic_change_lock(nullptr, &item3, &kTestCostModel, ctx);
  ASSERT_FALSE(r3.has_value());
  EXPECT_EQ(r3.error(), HogDBError(HogDBCode::kConflict));

  // Tx4 adds 101 but different height -> OK (New behavior: distinct uniquifier)
  PointerItem<ReplayCacheIt> item4(u(height_ + 2, 101));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item4, &kTestCostModel, ctx)
                  .has_value());
}

TEST_F(ReplayCacheLockTest, UndoMerge)
{
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_base(lock, base_data_);
  auto ctx = make_dummy_context(height_);

  // 1. Acquire 101
  PointerItem<ReplayCacheIt> item1(u(height_ + 1, 101));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, &kTestCostModel, ctx)
                  .has_value());

  // 2. Try to acquire 101 again -> Conflict
  PointerItem<ReplayCacheIt> item2(u(height_ + 1, 101));
  auto r2 = lock.try_atomic_change_lock(nullptr, &item2, &kTestCostModel, ctx);
  ASSERT_FALSE(r2.has_value());
  EXPECT_EQ(r2.error(), HogDBError(HogDBCode::kConflict));

  // 3. Release 101 (Undo Merge)
  ASSERT_TRUE(lock.try_atomic_change_lock(&item1, nullptr, &kTestCostModel, ctx)
                  .has_value());

  // 4. Try to acquire 101 again -> Success
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item2, &kTestCostModel, ctx)
                  .has_value());

  // 5. Complex case: Acquire 103 and 104
  PointerItem<ReplayCacheIt> item3(u(height_ + 1, 103));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item3, &kTestCostModel, ctx)
                  .has_value());

  PointerItem<ReplayCacheIt> item4(u(height_ + 1, 104));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item4, &kTestCostModel, ctx)
                  .has_value());

  // 6. Verify conflicts
  PointerItem<ReplayCacheIt> item3_dup(u(height_ + 1, 103));
  ASSERT_FALSE(
      lock.try_atomic_change_lock(nullptr, &item3_dup, &kTestCostModel, ctx)
          .has_value());

  PointerItem<ReplayCacheIt> item4_dup(u(height_ + 1, 104));
  ASSERT_FALSE(
      lock.try_atomic_change_lock(nullptr, &item4_dup, &kTestCostModel, ctx)
          .has_value());

  // 7. Release 103
  ASSERT_TRUE(lock.try_atomic_change_lock(&item3, nullptr, &kTestCostModel, ctx)
                  .has_value());

  // 8. Verify 103 success, 104 still conflict
  ASSERT_TRUE(
      lock.try_atomic_change_lock(nullptr, &item3_dup, &kTestCostModel, ctx)
          .has_value());
  ASSERT_FALSE(
      lock.try_atomic_change_lock(nullptr, &item4_dup, &kTestCostModel, ctx)
          .has_value());
}

TEST_F(ReplayCacheLockTest, Expiry)
{
  {
    ReplayCacheLock lock;
    ReplayCacheLockTestHelper::set_base(lock, base_data_);
    auto ctx = make_dummy_context(height_);

    // u(height_, 101) -> expiry 9. Current height 9. Allowed (>=).
    PointerItem<ReplayCacheIt> item1(u(height_, 101));
    ASSERT_TRUE(
        lock.try_atomic_change_lock(nullptr, &item1, &kTestCostModel, ctx)
            .has_value());

    // u(height_ + 1, 100) -> expiry 10. Base has u(10, 100). Conflict.
    PointerItem<ReplayCacheIt> item2(u(height_ + 1, 100));
    ASSERT_FALSE(
        lock.try_atomic_change_lock(nullptr, &item2, &kTestCostModel, ctx)
            .has_value());

    // u(height_ + 1, 101) -> expiry 10. Allowed.
    PointerItem<ReplayCacheIt> item3(u(height_ + 1, 101));
    ASSERT_TRUE(
        lock.try_atomic_change_lock(nullptr, &item3, &kTestCostModel, ctx)
            .has_value());
  }

  height_++; // height is now 10

  {
    ReplayCacheLock lock;
    ReplayCacheLockTestHelper::set_base(lock, base_data_);
    auto ctx = make_dummy_context(height_);

    // u(height_, 101) -> expiry 10. Current height 10. Allowed.
    PointerItem<ReplayCacheIt> item1(u(height_, 101));
    ASSERT_TRUE(
        lock.try_atomic_change_lock(nullptr, &item1, &kTestCostModel, ctx)
            .has_value());

    // u(height_, 100) -> expiry 10. Base has u(10, 100). Conflict.
    PointerItem<ReplayCacheIt> item2(u(height_, 100));
    ASSERT_FALSE(
        lock.try_atomic_change_lock(nullptr, &item2, &kTestCostModel, ctx)
            .has_value());

    // u(height_ + 1, 100) -> expiry 11. Allowed.
    PointerItem<ReplayCacheIt> item3(u(height_ + 1, 100));
    ASSERT_TRUE(
        lock.try_atomic_change_lock(nullptr, &item3, &kTestCostModel, ctx)
            .has_value());

    // u(height_ + 1, 101) -> expiry 11. Allowed.
    PointerItem<ReplayCacheIt> item4(u(height_ + 1, 101));
    ASSERT_TRUE(
        lock.try_atomic_change_lock(nullptr, &item4, &kTestCostModel, ctx)
            .has_value());

    // Build result at height 10.
    // Base: u(10, 100) -> expiry 10 >= 10. Keep.
    // Additions:
    // u(10, 101) -> expiry 10 >= 10. Keep.
    // u(11, 100) -> expiry 11 >= 10. Keep.
    // u(11, 101) -> expiry 11 >= 10. Keep.
    ReplayCacheData out =
        ReplayCacheLockTestHelper::build_result(lock, height_);
    ASSERT_EQ(4, out.uniquifiers.size());
    EXPECT_EQ(100, out.uniquifiers[0].nonce); // u(10, 100)
    EXPECT_EQ(101, out.uniquifiers[1].nonce); // u(10, 101)
    EXPECT_EQ(100, out.uniquifiers[2].nonce); // u(11, 100)
    EXPECT_EQ(101, out.uniquifiers[3].nonce); // u(11, 101)
  }
}

TEST_F(ReplayCacheLockTest, Build)
{
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_base(lock, base_data_);
  auto ctx = make_dummy_context(height_);

  PointerItem<ReplayCacheIt> item1(u(height_, 102));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, &kTestCostModel, ctx)
                  .has_value());

  PointerItem<ReplayCacheIt> item2(u(height_, 101));
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &item2, &kTestCostModel, ctx)
                  .has_value());

  ReplayCacheData out = ReplayCacheLockTestHelper::build_result(lock, height_);
  ASSERT_EQ(3, out.uniquifiers.size());
  EXPECT_EQ(101, out.uniquifiers[0].nonce); // had height 9
  EXPECT_EQ(102, out.uniquifiers[1].nonce); // had height 9
  EXPECT_EQ(100, out.uniquifiers[2].nonce); // had height 10 (part of base)

  ReplayCacheLock remade;
  ReplayCacheLockTestHelper::set_base(remade, out);
  EXPECT_FALSE(
      remade.try_atomic_change_lock(nullptr, &item1, &kTestCostModel, ctx)
          .has_value());
  EXPECT_FALSE(
      remade.try_atomic_change_lock(nullptr, &item2, &kTestCostModel, ctx)
          .has_value());

  PointerItem<ReplayCacheIt> item3(u(height_ + 1, 100));
  EXPECT_FALSE(
      remade.try_atomic_change_lock(nullptr, &item3, &kTestCostModel, ctx)
          .has_value());
}

TEST_F(ReplayCacheLockTest, MergeCost)
{
  ReplayCacheLock lock;
  ReplayCacheLockTestHelper::set_base(lock, base_data_);
  auto ctx = make_dummy_context(height_);

  { // merging non-conflicting entry results adds cost
    PointerItem<ReplayCacheIt> item(u(height_ + 1, 101));
    auto r = lock.try_atomic_change_lock(nullptr, &item, &kTestCostModel, ctx);
    ASSERT_TRUE(r);
    EXPECT_EQ(kTestCostModel.get_replay_cache_cost(2), *r);
  }

  { // merging one more non-conflicting entry in requires
    // one more entry worth of cost to be locked
    PointerItem<ReplayCacheIt> item(u(height_ + 1, 102));
    auto r = lock.try_atomic_change_lock(nullptr, &item, &kTestCostModel, ctx);
    ASSERT_TRUE(r);
    EXPECT_EQ(kTestCostModel.get_replay_cache_cost(2), *r);
  }

  // checks that merges and costs work with uniquifier expiry are in HogDBTest
  // since they are best tested with storage.
}

} // namespace hogd
