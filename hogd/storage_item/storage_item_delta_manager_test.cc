#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/invalid_state_flag.h"
#include "hogd/storage_item/retrocausal_queue.h"
#include "hogd/storage_item/storage_item_delta_manager.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "hogd/storage_item/storage_lock_util.h"
#include "model_checker/async.h"
#include "model_checker/threadpool.h"
#include "model_checker/work_queue.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "types.hh"
#include "x23/format.h"
#include "x23/vec.h"

namespace hogd {

// ASSERT_TRUE(x) doesn't show up to clangd as a check for the optional
// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace {

const StorageCostModel cost_model(12000, 2, 5, 100, 5);

void
expect_final(StorageItemDeltaManager &item, Bytestring const &bytestring)
{
  auto snapshot = item.get_post_close_block_item();
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->type(), StorageItemType::kItemTypeBytestring);
  EXPECT_EQ(snapshot->bytestring(), bytestring);
}
void
expect_final(StorageItemDeltaManager &item, int128_t nnint)
{
  auto snapshot = item.get_post_close_block_item();
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->type(), StorageItemType::kItemTypeNonnegativeInt);
  EXPECT_EQ(snapshot->nnint(), nnint);
}

void
expect_final(StorageItemDeltaManager &item, RetrocausalQueueStats stats)
{
  auto snapshot = item.get_post_close_block_item();
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->type(), StorageItemType::kItemTypeRetrocausalQueue);
  EXPECT_EQ(snapshot->queue().get_active_stats(), stats);
}

auto
make_bounds_lock(int128_t add, int128_t sub)
{
  return NNIntBoundsLock{.add_amount = to_xdr(add),
                         .subtract_amount = to_xdr(sub)};
}

auto
make_nnint_delta(int128_t set, int128_t add)
{
  StorageDelta delta(StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd);
  delta.nnint().set_value = to_xdr(set);
  delta.nnint().add_value = to_xdr(add);
  return delta;
};

auto
make_nnint_add_delta(int128_t add)
{
  StorageDelta delta(StorageDeltaType::kDeltaTypeNonnegativeIntAdd);
  delta.nnint_add() = to_xdr(add);
  return delta;
}

auto
make_replay_delta(x23::Vec<ReplayNonce> nonces, ReplayNonce minimum_threshold)
{
  StorageDelta delta(StorageDeltaType::kDeltaTypeReplayCacheModify);
  delta.replay_cache_modify() = {
      .minimum_threshold = minimum_threshold,
      .nonces = std::move(nonces),
  };
  return delta;
};

} // namespace

class DeltaManagerTest : public ::testing::TestWithParam<BlockExecutionMode> {
protected:
  void SetUp() override { mode_ = GetParam(); }

  BlockExecutionMode mode_;
  InvalidStateFlag flag_;
};

// Change the typeclass of a storage item during execution
TEST_P(DeltaManagerTest, RewriteVariantTypeNullStart)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag, mode_,
                               &cost_model);

  StorageDelta bytestring_delta(StorageDeltaType::kDeltaTypeBytestringSet);
  StorageDelta nnint_set_add_1 = make_nnint_delta(0, 10);
  StorageDelta nnint_set_add_2 = make_nnint_delta(5, 10);
  StorageDelta nnint_set_add_3 = make_nnint_delta(5, -3);

  auto b_lock =
      item.try_reserve(bytestring_delta, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(b_lock);

  // type mismatch
  EXPECT_FALSE(
      item.try_reserve(nnint_set_add_1, StorageLock(StorageLockType::kNone)));
  item.revert(b_lock->lock_);

  // item in neutral state
  auto nnint_lock_1 =
      item.try_reserve(nnint_set_add_1, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(nnint_lock_1);
  // mismatch in nnint set value
  EXPECT_FALSE(
      item.try_reserve(nnint_set_add_2, StorageLock(StorageLockType::kNone)));
  item.revert(nnint_lock_1->lock_);

  // item in neutral state
  auto nnint_lock_2 =
      item.try_reserve(nnint_set_add_2, StorageLock(StorageLockType::kNone));
  auto nnint_lock_3 =
      item.try_reserve(nnint_set_add_3, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(nnint_lock_2);
  ASSERT_TRUE(nnint_lock_3);

  if (mode_ == BlockExecutionMode::kBlockExecutionModeProposal) {
    // fails because too much subtraction
    EXPECT_FALSE(
        item.try_reserve(nnint_set_add_3, StorageLock(StorageLockType::kNone)));
  }
  item.revert(nnint_lock_3->lock_);

  nnint_lock_3 =
      item.try_reserve(nnint_set_add_3, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(nnint_lock_3);

  item.revert(nnint_lock_3->lock_);
  item.revert(nnint_lock_2->lock_);
  // item in neutral state

  EXPECT_TRUE(
      item.try_reserve(bytestring_delta, StorageLock(StorageLockType::kNone)));

  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST_P(DeltaManagerTest, RewriteVariantFromBase)
{
  auto base_item =
      std::make_shared<StorageItemWrapper>(Bytestring{0, 1, 2, 3}, &cost_model);
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(base_item, &invalid_state_flag, mode_,
                               &cost_model);

  StorageDelta bytestring_delta(StorageDeltaType::kDeltaTypeBytestringSet);
  bytestring_delta.bytestring() = {3, 2, 1, 0};

  StorageDelta nnint_set_add = make_nnint_delta(0, 10);

  // updated delete semantics allows changing type:
  // checks for mismatched writes go in transaction runtime,
  // not the storage item frame
  auto nnint_lock =
      item.try_reserve(nnint_set_add, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(nnint_lock);

  EXPECT_FALSE(
      item.try_reserve(bytestring_delta, StorageLock(StorageLockType::kNone)));
  item.revert(nnint_lock->lock_);

  // In neutral state now

  auto b_lock =
      item.try_reserve(bytestring_delta, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(b_lock);

  EXPECT_FALSE(
      item.try_reserve(nnint_set_add, StorageLock(StorageLockType::kNone)));
  auto b_lock2 =
      item.try_reserve(bytestring_delta, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(b_lock2);

  item.commit(bytestring_delta, *b_lock);
  item.commit(bytestring_delta, *b_lock2);
  ASSERT_FALSE(invalid_state_flag.is_invalid());

  StorageItemDeltaManager item2(item.get_post_close_block_item(),
                                &invalid_state_flag, mode_, &cost_model);
  expect_final(item, bytestring_delta.bytestring());
  expect_final(item2, bytestring_delta.bytestring());
  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST_P(DeltaManagerTest, DeleteItem)
{
  InvalidStateFlag invalid_state_flag;
  auto base_item =
      std::make_shared<StorageItemWrapper>(Bytestring{0, 1, 2, 3}, &cost_model);

  StorageItemDeltaManager item(base_item, &invalid_state_flag, mode_,
                               &cost_model);
  ASSERT_TRUE(item.get_snapshot_item());

  // committing an empty round keeps item active
  StorageItemDeltaManager item2(item.get_post_close_block_item(),
                                &invalid_state_flag, mode_, &cost_model);

  ASSERT_TRUE(item2.get_snapshot_item());

  auto delete_lock =
      item2.try_reserve(StorageDelta(StorageDeltaType::kDeltaTypeDelete),
                        StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(delete_lock);

  StorageDelta bytestring_delta(StorageDeltaType::kDeltaTypeBytestringSet);
  bytestring_delta.bytestring() = {3, 2, 1, 0};
  EXPECT_FALSE(
      item2.try_reserve(bytestring_delta, StorageLock(StorageLockType::kNone)));

  item2.commit(StorageDelta(StorageDeltaType::kDeltaTypeDelete), *delete_lock);
  ASSERT_FALSE(invalid_state_flag.is_invalid());
  ASSERT_FALSE(item2.get_post_close_block_item()); // item2 has the item deleted
                                                   // post block

  StorageItemDeltaManager item3(item2.get_post_close_block_item(),
                                &invalid_state_flag, mode_, &cost_model);

  EXPECT_FALSE(item3.get_snapshot_item());
  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST_P(DeltaManagerTest, ExclusiveLockAcquire)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag, mode_,
                               &cost_model);

  auto lock1 =
      item.try_reserve(StorageDelta(StorageDeltaType::kDeltaTypeDelete),
                       StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lock1);

  StorageDelta bytestring_delta(StorageDeltaType::kDeltaTypeBytestringSet);
  bytestring_delta.bytestring() = {3, 2, 1, 0};

  auto lock2 = item.try_reserve(bytestring_delta, lock1->lock_);
  ASSERT_TRUE(lock2);
  EXPECT_EQ(lock2->lock_.lock_type(), StorageLockType::kExclusiveLock);

  item.commit(bytestring_delta, *lock2);

  expect_final(item, bytestring_delta.bytestring());
  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST_P(DeltaManagerTest, NNIntSharedLock2SubSecondFails)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag, mode_,
                               &cost_model);

  StorageDelta nnint_set_add_1 = make_nnint_delta(5, 10);
  StorageDelta nnint_set_add_2 = make_nnint_delta(5, -4);
  StorageDelta nnint_set_add_3 = make_nnint_delta(5, -3);
  StorageDelta nnint_set_add_4 = make_nnint_delta(5, -5);

  // Logic about set_add of a positive value offsetting a later negative value
  // is handled in the runtime, not here.

  auto lock1 =
      item.try_reserve(nnint_set_add_1, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lock1);

  auto lock2 = item.try_reserve(nnint_set_add_2, lock1->lock_);
  ASSERT_TRUE(lock2);

  auto lock3 = item.try_reserve(nnint_set_add_3, lock2->lock_);
  ASSERT_TRUE(lock3);

  ASSERT_EQ(*lock3, *lock2);

  ASSERT_EQ(lock3->lock_.lock_type(), StorageLockType::kSetItemTypeClass);

  EXPECT_EQ(lock3->lock_.full_typeclass_lock().typeclass.item_type(),
            StorageItemTypeClass::kItemTypeNonnegativeInt);
  if (mode_ == BlockExecutionMode::kBlockExecutionModeProposal) {
    EXPECT_EQ(
        lock3->lock_.full_typeclass_lock().extra_lock_data.nnint_bounds_lock(),
        make_bounds_lock(int128_t{10}, int128_t{-4}));
  }

  auto lock4 = item.try_reserve(nnint_set_add_4, lock3->lock_);
  ASSERT_TRUE(lock4);

  EXPECT_EQ(lock4->lock_.lock_type(), StorageLockType::kSetItemTypeClass);
  EXPECT_EQ(lock4->lock_.full_typeclass_lock().typeclass.item_type(),
            StorageItemTypeClass::kItemTypeNonnegativeInt);
  if (mode_ == BlockExecutionMode::kBlockExecutionModeProposal) {
    EXPECT_EQ(
        lock4->lock_.full_typeclass_lock().extra_lock_data.nnint_bounds_lock(),
        make_bounds_lock(int128_t{10}, int128_t{-5}));
  }

  // return to nnint_set_add_2
  auto final_lock = item.try_reserve(nnint_set_add_2, lock4->lock_);
  ASSERT_TRUE(final_lock);

  item.commit(nnint_set_add_2, *final_lock);

  expect_final(item, int128_t{1});
  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST_P(DeltaManagerTest, NNIntFromExclusiveLock)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag, mode_,
                               &cost_model);

  StorageDelta nnint_set_add_1 = make_nnint_delta(5, 10);
  StorageDelta nnint_set_add_2 = make_nnint_delta(10, -4);

  auto lock1 =
      item.try_reserve(nnint_set_add_1, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lock1);

  auto lock2 = item.try_reserve(nnint_set_add_2, lock1->lock_);
  ASSERT_TRUE(lock2);

  EXPECT_EQ(lock2->lock_.lock_type(), StorageLockType::kExclusiveLock);

  // check to assert only one exclusive lock can be given out.
  EXPECT_FALSE(
      item.try_acquire_exclusive_lock(nnint_set_add_1, StorageLockType::kNone,
                                      StorageLockType::kExclusiveLock));
  item.commit(nnint_set_add_2, *lock2);
  EXPECT_FALSE(
      item.try_acquire_exclusive_lock(nnint_set_add_1, StorageLockType::kNone,
                                      StorageLockType::kExclusiveLock));

  expect_final(item, int128_t{6});
  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST_P(DeltaManagerTest, NNIntFromExclusiveLockNegativeSetValue)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag, mode_,
                               &cost_model);

  StorageDelta nnint_set_add_1 = make_nnint_delta(-5, 10);
  StorageDelta nnint_set_add_2 = make_nnint_delta(-10, 4);

  auto lock1 =
      item.try_reserve(nnint_set_add_1, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lock1);

  auto lock2 = item.try_reserve(nnint_set_add_2, lock1->lock_);
  ASSERT_TRUE(lock2);

  EXPECT_EQ(lock2->lock_.lock_type(), StorageLockType::kExclusiveLock);

  // check to assert only one exclusive lock can be given out.
  EXPECT_FALSE(
      item.try_acquire_exclusive_lock(nnint_set_add_1, StorageLockType::kNone,
                                      StorageLockType::kExclusiveLock));
  item.commit(nnint_set_add_2, *lock2);
  EXPECT_FALSE(
      item.try_acquire_exclusive_lock(nnint_set_add_1, StorageLockType::kNone,
                                      StorageLockType::kExclusiveLock));

  expect_final(item, int128_t{-6});
  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST_P(DeltaManagerTest, ExclusiveBytestring)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag, mode_,
                               &cost_model);

  StorageDelta bytestring_delta(StorageDeltaType::kDeltaTypeBytestringSet);
  bytestring_delta.bytestring() = {3, 2, 1, 0};

  auto res =
      item.try_acquire_exclusive_lock(bytestring_delta, StorageLockType::kNone,
                                      StorageLockType::kPermanentExclusiveLock);
  ASSERT_TRUE(res);

  auto lock = item.try_reserve(bytestring_delta, res->lock_);
  ASSERT_TRUE(lock);

  item.commit(bytestring_delta, *lock);

  ASSERT_FALSE(
      item.try_acquire_exclusive_lock(bytestring_delta, StorageLockType::kNone,
                                      StorageLockType::kExclusiveLock));
  ASSERT_FALSE(
      item.try_reserve(bytestring_delta, StorageLock(StorageLockType::kNone)));

  expect_final(item, bytestring_delta.bytestring());
  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST_P(DeltaManagerTest, ExclusiveLockMultiAcquire)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag, mode_,
                               &cost_model);

  // This is just a placeholder for the test
  StorageDelta bytestring_delta(StorageDeltaType::kDeltaTypeBytestringSet);
  bytestring_delta.bytestring() = {3, 2, 1, 0};

  ASSERT_TRUE(item.try_acquire_exclusive_lock(bytestring_delta,
                                              StorageLockType::kNone,
                                              StorageLockType::kExclusiveLock));

  ASSERT_FALSE(
      item.try_acquire_exclusive_lock(bytestring_delta, StorageLockType::kNone,
                                      StorageLockType::kExclusiveLock));
  // Should never give out an exclusive lock and a SetItemTypeClass lock.
  // But can acquire exclusive if you already have exclusive
  ASSERT_TRUE(item.try_acquire_exclusive_lock(bytestring_delta,
                                              StorageLockType::kExclusiveLock,
                                              StorageLockType::kExclusiveLock));
  auto lock =
      item.try_acquire_exclusive_lock(bytestring_delta,
                                      StorageLockType::kPermanentExclusiveLock,
                                      StorageLockType::kExclusiveLock);
  ASSERT_TRUE(lock);

  item.commit(StorageDelta(StorageDeltaType::kDeltaTypeDelete), *lock);

  ASSERT_FALSE(
      item.try_acquire_exclusive_lock(bytestring_delta, StorageLockType::kNone,
                                      StorageLockType::kExclusiveLock));
  ASSERT_FALSE(item.try_reserve(
      StorageDelta(StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd),
      StorageLock(StorageLockType::kNone)));

  // exclusive lock was committed but released, so we can acquire shared lock.
  // Was not written in permanent exclusive mode.
  ASSERT_TRUE(item.try_reserve(StorageDelta(StorageDeltaType::kDeltaTypeDelete),
                               StorageLock(StorageLockType::kNone)));

  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST_P(DeltaManagerTest, PermanentExclusiveLockMultiAcquire)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag,
                               BlockExecutionMode::kBlockExecutionModeProposal,
                               &cost_model);

  // merely a placeholder for the test
  StorageDelta bytestring_delta(StorageDeltaType::kDeltaTypeBytestringSet);
  bytestring_delta.bytestring() = {3, 2, 1, 0};

  ASSERT_TRUE(item.try_acquire_exclusive_lock(bytestring_delta,
                                              StorageLockType::kNone,
                                              StorageLockType::kExclusiveLock));

  ASSERT_FALSE(
      item.try_acquire_exclusive_lock(bytestring_delta, StorageLockType::kNone,
                                      StorageLockType::kExclusiveLock));
  // Should never give out an exclusive lock and a SetItemTypeClass lock.
  auto lock = (item.try_acquire_exclusive_lock(
      bytestring_delta, StorageLockType::kPermanentExclusiveLock,
      StorageLockType::kPermanentExclusiveLock));
  ASSERT_TRUE(lock);

  item.commit(StorageDelta(StorageDeltaType::kDeltaTypeDelete), *lock);

  ASSERT_FALSE(
      item.try_acquire_exclusive_lock(bytestring_delta, StorageLockType::kNone,
                                      StorageLockType::kExclusiveLock));
  ASSERT_FALSE(item.try_reserve(
      StorageDelta(StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd),
      StorageLock(StorageLockType::kNone)));

  // cannot reacquire lock after permament lock commits
  ASSERT_FALSE(
      item.try_reserve(StorageDelta(StorageDeltaType::kDeltaTypeDelete),
                       StorageLock(StorageLockType::kNone)));
  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST_P(DeltaManagerTest, NNIntSimpleSetAdd)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag, mode_,
                               &cost_model);

  StorageDelta nnint_set_add = make_nnint_delta(5, 10);

  auto lock =
      item.try_reserve(nnint_set_add, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lock);

  item.commit(nnint_set_add, *lock);

  expect_final(item, int128_t{15});
  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST_P(DeltaManagerTest, ExclusiveLockCostMatchNoPriorCommit)
{
  auto make_bytestring_delta = [](x23::Vec<uint8_t> vec) {
    StorageDelta delta(StorageDeltaType::kDeltaTypeBytestringSet);
    delta.bytestring() = std::move(vec);
    return delta;
  };

  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag,
                               BlockExecutionMode::kBlockExecutionModeProposal,
                               &cost_model);

  auto bdelta_4 = make_bytestring_delta({0, 1, 2, 3});
  auto bdelta_6 = make_bytestring_delta({0, 1, 2, 3, 4, 5});

  auto lk_clean =
      item.try_reserve(bdelta_4, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk_clean);

  item.revert(lk_clean->lock_);

  auto lk_exclusive =
      item.try_acquire_exclusive_lock(bdelta_4, StorageLockType::kNone,
                                      StorageLockType::kExclusiveLock);
  ASSERT_TRUE(lk_exclusive);
  ASSERT_EQ(lk_clean->locked_tokens_, lk_exclusive->locked_tokens_);

  auto lk_6 = item.try_reserve(bdelta_6, lk_exclusive->lock_);
  ASSERT_TRUE(lk_6);

  ASSERT_EQ(lk_6->locked_tokens_,
            lk_exclusive->locked_tokens_ + cost_model.get_bytestring_cost(2));
}

TEST_P(DeltaManagerTest, NNIntSetAddInvalidState)
{
  if (mode_ == BlockExecutionMode::kBlockExecutionModeProposal) {
    // This test is only valid for validation mode
    return;
  }
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag, mode_,
                               &cost_model);

  // drive invalid and then back to valid

  auto nnint_set_add_pos = make_nnint_delta(0, 10);
  auto nnint_set_add_neg = make_nnint_delta(0, -6);

  auto lk_pos =
      item.try_reserve(nnint_set_add_pos, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk_pos);
  item.commit(nnint_set_add_pos, *lk_pos);

  ASSERT_FALSE(invalid_state_flag.is_invalid());

  auto lk_neg =
      item.try_reserve(nnint_set_add_neg, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk_neg);
  item.commit(nnint_set_add_neg, *lk_neg);

  ASSERT_FALSE(invalid_state_flag.is_invalid());

  auto lk_neg2 =
      item.try_reserve(nnint_set_add_neg, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk_neg2);
  item.commit(nnint_set_add_neg, *lk_neg2);

  ASSERT_TRUE(invalid_state_flag.is_invalid());

  auto lk_neg3 =
      item.try_reserve(nnint_set_add_neg, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk_neg3);
  item.commit(nnint_set_add_neg, *lk_neg3);

  ASSERT_TRUE(invalid_state_flag.is_invalid());

  auto lk_pos2 =
      item.try_reserve(nnint_set_add_pos, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk_pos2);
  item.commit(nnint_set_add_pos, *lk_pos2);

  ASSERT_FALSE(invalid_state_flag.is_invalid());

  expect_final(item, int128_t{2});
}

TEST_P(DeltaManagerTest, NNIntSetAddNegativeSet)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag, mode_,
                               &cost_model);

  ASSERT_FALSE(item.try_reserve(make_nnint_delta(-10, -1),
                                StorageLock(StorageLockType::kNone)));

  auto nnint_set_add0 = make_nnint_delta(-10, 0);
  auto lk0 =
      item.try_reserve(nnint_set_add0, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk0);
  item.commit(nnint_set_add0, *lk0);

  ASSERT_FALSE(invalid_state_flag.is_invalid());

  auto nnint_set_add1 = make_nnint_delta(-10, 1);
  auto lk1 =
      item.try_reserve(nnint_set_add1, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk1);
  item.commit(nnint_set_add1, *lk1);

  ASSERT_FALSE(invalid_state_flag.is_invalid());

  ASSERT_FALSE(item.try_reserve(make_nnint_delta(-10, -1),
                                StorageLock(StorageLockType::kNone)));

  expect_final(item, int128_t{-9});
}

TEST_P(DeltaManagerTest, NNIntRetrocausalityInProposalMode)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag, mode_,
                               &cost_model);

  auto nnint_set_add_pos = make_nnint_delta(0, 10);
  auto nnint_set_add_neg = make_nnint_delta(0, -6);

  auto lk_pos =
      item.try_reserve(nnint_set_add_pos, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk_pos);
  item.commit(nnint_set_add_pos, *lk_pos);
  // value is 10

  auto lk_neg =
      item.try_reserve(nnint_set_add_neg, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk_neg);
  item.commit(nnint_set_add_neg, *lk_neg);
  // value is 4

  if (mode_ == BlockExecutionMode::kBlockExecutionModeProposal) {
    ASSERT_FALSE(item.try_reserve(nnint_set_add_neg,
                                  StorageLock(StorageLockType::kNone)));
  }

  lk_pos =
      item.try_reserve(nnint_set_add_pos, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk_pos);
  item.commit(nnint_set_add_pos, *lk_pos);
  // value is 14

  lk_neg =
      item.try_reserve(nnint_set_add_neg, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk_neg);
  item.commit(nnint_set_add_neg, *lk_neg);
  // value is 8

  lk_neg =
      item.try_reserve(nnint_set_add_neg, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk_neg);
  item.commit(nnint_set_add_neg, *lk_neg);
  // value is 2

  expect_final(item, int128_t{2});
}

TEST_P(DeltaManagerTest, ReplayCacheInsertFromEmpty)
{
  StorageItemDeltaManager item(nullptr, &flag_, mode_, &cost_model);

  auto delta1 = make_replay_delta({1, 2, 3}, 0);
  auto delta2 = make_replay_delta({4, 5, 6}, 4);
  // conflicts with delta1 and delta2
  auto delta3 = make_replay_delta({1, 4, 7}, 5);

  auto delta4 = make_replay_delta(
      {
          8,
          9,
          10,
      },
      7);

  auto lock1 = item.try_reserve(delta1, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lock1);

  EXPECT_FALSE(item.try_reserve(delta3, StorageLock(StorageLockType::kNone)));

  auto lock2 = item.try_reserve(delta2, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lock2);

  item.commit(delta1, *lock1);
  item.commit(delta2, *lock2);

  EXPECT_FALSE(item.try_reserve(delta3, StorageLock(StorageLockType::kNone)));

  auto lock4 = item.try_reserve(delta4, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lock4);
  item.revert(lock4->lock_);

  auto r = item.get_post_close_block_item();
  ASSERT_TRUE(r);
  EXPECT_EQ(r->type(), StorageItemType::kItemTypeReplayCache);
  EXPECT_EQ(r->replay_cache().metadata().minimum_threshold, 4);
  EXPECT_EQ(r->replay_cache().nonces(), (std::set<ReplayNonce>{4, 5, 6}));
}

namespace {
auto
make_test_rcq_entry(uint8_t value)
{
  return QueueEntry{value};
}
auto
make_rcq_delta(std::optional<RetrocausalQueueConsumeDelta> consume_delta,
               RetrocausalQueueInsertDelta insert_delta)
{
  StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
  if (consume_delta) {
    delta.retrocausal_queue().consume.emplace(*consume_delta);
  }
  delta.retrocausal_queue().insert = std::move(insert_delta);
  return delta;
}
} // namespace

TEST_P(DeltaManagerTest, RCQInsertThenConsume)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag, mode_,
                               &cost_model);

  // try insert one entry
  auto insert_delta =
      make_rcq_delta(std::nullopt,
                     RetrocausalQueueInsertDelta{
                         .added_entries = {make_test_rcq_entry(1)}});
  auto lk = item.try_reserve(insert_delta, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk);
  EXPECT_EQ(lk->locked_tokens_,
            cost_model.kBaseItemCost +
                cost_model.get_retrocausal_queue_cost(1, 1));
  item.commit(insert_delta, *lk);

  // try consume one entry
  auto consume_delta = make_rcq_delta(
      RetrocausalQueueConsumeDelta{
          .past_block_consume =
              RetrocausalQueuePastBlockConsumeDelta{
                  .max_fully_consumed_block_number = 0,
                  .partial_consumed_number = 0},
          .current_block_consumed = {make_test_rcq_entry(1)}},
      RetrocausalQueueInsertDelta{.added_entries = {}});
  auto lk2 =
      item.try_reserve(consume_delta, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk2);
  EXPECT_EQ(lk2->locked_tokens_, 0);
  item.commit(consume_delta, *lk2);

  expect_final(item, RetrocausalQueueStats{.active_block_count = 0,
                                           .active_size = 0});
}

TEST_P(DeltaManagerTest, RCQConsumeThenInsert)
{
  if (mode_ == BlockExecutionMode::kBlockExecutionModeProposal) {
    // This test is only valid for validation mode
    return;
  }

  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag, mode_,
                               &cost_model);

  // try consume one entry
  auto consume_delta = make_rcq_delta(
      RetrocausalQueueConsumeDelta{
          .past_block_consume =
              RetrocausalQueuePastBlockConsumeDelta{
                  .max_fully_consumed_block_number = 0,
                  .partial_consumed_number = 0},
          .current_block_consumed = {make_test_rcq_entry(1)}},
      RetrocausalQueueInsertDelta{.added_entries = {}});
  auto lk2 =
      item.try_reserve(consume_delta, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk2);
  EXPECT_GE(lk2->locked_tokens_,
            cost_model.kBaseItemCost +
                cost_model.get_retrocausal_queue_cost(-1, 0));
  item.commit(consume_delta, *lk2);

  // try insert one entry
  auto insert_delta =
      make_rcq_delta(std::nullopt,
                     RetrocausalQueueInsertDelta{
                         .added_entries = {make_test_rcq_entry(1)}});
  auto lk = item.try_reserve(insert_delta, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk);
  item.commit(insert_delta, *lk);

  ASSERT_FALSE(invalid_state_flag.is_invalid());

  expect_final(item, RetrocausalQueueStats{.active_block_count = 0,
                                           .active_size = 0});
}

TEST_P(DeltaManagerTest, NNIntInterleavings)
{
  InvalidStateFlag invalid_state_flag;

  model::ThreadPool<StorageItemDeltaManager, int128_t> pool(4);

  std::shared_ptr<model::ExperimentBuilder<StorageItemDeltaManager, int128_t>>
      experiment = std::make_shared<
          model::ExperimentBuilder<StorageItemDeltaManager, int128_t>>(
          [&]() {
            return std::make_tuple(StorageItemDeltaManager(nullptr,
                                                           &invalid_state_flag,
                                                           mode_, &cost_model),
                                   int128_t{0});
          },
          [](model::WorkQueue &work_queue, StorageItemDeltaManager &item,
             int128_t &net_storage_cost) {
            auto actions =
                std::make_unique<model::RunnableActionSet>(work_queue);

            // set_add (0, 10)
            actions->add_action(
                [](model::RunnableActionSet &set, StorageItemDeltaManager &item,
                   int128_t &net_storage_cost) -> model::Async {
                  co_await set.bg();
                  auto nnint_add = make_nnint_delta(0, 10);
                  auto lk =
                      item.try_reserve(nnint_add,
                                       StorageLock(StorageLockType::kNone));
                  if (!lk) {
                    throw std::runtime_error("Failed to reserve lock");
                  }
                  net_storage_cost += lk->locked_tokens_;
                  co_await set.bg();
                  auto res = item.commit(nnint_add, *lk);
                  net_storage_cost -= res;
                },
                item, net_storage_cost);

            // set_add (0, 15)
            actions->add_action(
                [](model::RunnableActionSet &set, StorageItemDeltaManager &item,
                   int128_t &net_storage_cost) -> model::Async {
                  co_await set.bg();
                  auto nnint_add = make_nnint_delta(0, 15);
                  auto lk =
                      item.try_reserve(nnint_add,
                                       StorageLock(StorageLockType::kNone));
                  if (!lk) {
                    throw std::runtime_error("Failed to reserve lock");
                  }
                  net_storage_cost += lk->locked_tokens_;
                  co_await set.bg();
                  auto res = item.commit(nnint_add, *lk);
                  net_storage_cost -= res;
                },
                item, net_storage_cost);

            return actions;
          },
          [](model::ActionResult res, StorageItemDeltaManager &item,
             int128_t &net_storage_cost) -> bool {
            if (res != model::ActionResult::kOk) {
              return false;
            }

            if (net_storage_cost != cost_model.kBaseItemCost) {
              return false;
            }

            auto snapshot = item.get_post_close_block_item();
            if (!snapshot ||
                snapshot->type() != StorageItemType::kItemTypeNonnegativeInt) {
              return false;
            }

            return snapshot->nnint() == 25;
          });

  EXPECT_TRUE(pool.run_test(experiment));
}

TEST_P(DeltaManagerTest, NNIntConflictingInterleavings)
{
  InvalidStateFlag invalid_state_flag;

  model::ThreadPool<StorageItemDeltaManager, int128_t> pool(4);

  std::shared_ptr<model::ExperimentBuilder<StorageItemDeltaManager, int128_t>>
      experiment = std::make_shared<
          model::ExperimentBuilder<StorageItemDeltaManager, int128_t>>(
          [&]() {
            return std::make_tuple(StorageItemDeltaManager(nullptr,
                                                           &invalid_state_flag,
                                                           mode_, &cost_model),
                                   int128_t{0});
          },
          [](model::WorkQueue &work_queue, StorageItemDeltaManager &item,
             int128_t &net_storage_cost) {
            auto actions =
                std::make_unique<model::RunnableActionSet>(work_queue);

            // set_add (10, 10)
            actions->add_action(
                [](model::RunnableActionSet &set, StorageItemDeltaManager &item,
                   int128_t &net_storage_cost) -> model::Async {
                  co_await set.bg();
                  auto nnint_add = make_nnint_delta(10, 10);
                  auto lk =
                      item.try_reserve(nnint_add,
                                       StorageLock(StorageLockType::kNone));
                  if (!lk) {
                    co_return;
                  }
                  net_storage_cost += lk->locked_tokens_;
                  co_await set.bg();
                  auto res = item.commit(nnint_add, *lk);
                  net_storage_cost -= res;
                },
                item, net_storage_cost);

            // set_add (0, 15)
            actions->add_action(
                [](model::RunnableActionSet &set, StorageItemDeltaManager &item,
                   int128_t &net_storage_cost) -> model::Async {
                  co_await set.bg();
                  auto nnint_add = make_nnint_delta(0, 15);
                  auto lk =
                      item.try_reserve(nnint_add,
                                       StorageLock(StorageLockType::kNone));
                  if (!lk) {
                    co_return;
                  }
                  net_storage_cost += lk->locked_tokens_;
                  co_await set.bg();
                  auto res = item.commit(nnint_add, *lk);
                  net_storage_cost -= res;
                },
                item, net_storage_cost);

            actions->add_action(
                [](model::RunnableActionSet &set, StorageItemDeltaManager &item,
                   int128_t &net_storage_cost) -> model::Async {
                  co_await set.bg();
                  auto nnint_add = make_nnint_delta(10, -10);
                  auto lk =
                      item.try_reserve(nnint_add,
                                       StorageLock(StorageLockType::kNone));
                  if (!lk) {
                    co_return;
                  }
                  net_storage_cost += lk->locked_tokens_;
                  co_await set.bg();
                  auto res = item.commit(nnint_add, *lk);
                  net_storage_cost -= res;
                },
                item, net_storage_cost);

            return actions;
          },
          [](model::ActionResult res, StorageItemDeltaManager &item,
             int128_t &net_storage_cost) -> bool {
            if (res != model::ActionResult::kOk) {
              return false;
            }

            if (net_storage_cost != cost_model.kBaseItemCost) {
              return false;
            }

            auto snapshot = item.get_post_close_block_item();
            if (!snapshot ||
                snapshot->type() != StorageItemType::kItemTypeNonnegativeInt) {
              return false;
            }

            // either both the (10, 10) and (10, -10) are committed,
            // or only the (0, 15) is committed.
            return snapshot->nnint() == 10 || snapshot->nnint() == 15;
          });

  EXPECT_TRUE(pool.run_test(experiment));
}

// The test case that was problematic before the switch to upper bounding costs:
// Reserve A insert(1) -- reserve 1 block
// Commit A -- release 0
// Reserve B insert (1) -- on old workflow, this would reserve 0 blocks; here it
// reserves 1
// Reserve C consume(1) -- reserve 0 blocks
// Commit C -- on old workflow, has to release 1 block (B might not commit)
// then Commit B is stuck -- it has no reservation, but it has to pay for
// creating a block

TEST_P(DeltaManagerTest, RCQAccountingEdgeCase)
{
  InvalidStateFlag invalid_state_flag;
  model::ThreadPool<StorageItemDeltaManager, int128_t> pool(4);
  std::shared_ptr<model::ExperimentBuilder<StorageItemDeltaManager, int128_t>>
      experiment = std::make_shared<
          model::ExperimentBuilder<StorageItemDeltaManager, int128_t>>(
          [&]() {
            return std::make_tuple(StorageItemDeltaManager(nullptr,
                                                           &invalid_state_flag,
                                                           mode_, &cost_model),
                                   int128_t{0});
          },
          [](model::WorkQueue &work_queue, StorageItemDeltaManager &item,
             int128_t &net_storage_cost) {
            auto actions =
                std::make_unique<model::RunnableActionSet>(work_queue, 10);

            // Reserve A insert(1)
            actions->add_action(
                [](model::RunnableActionSet &set, StorageItemDeltaManager &item,
                   int128_t &net_storage_cost) -> model::Async {
                  co_await set.bg();
                  auto insert_a =
                      make_rcq_delta(std::nullopt,
                                     RetrocausalQueueInsertDelta{
                                         .added_entries = {
                                             make_test_rcq_entry(1)}});
                  auto lk_a =
                      item.try_reserve(insert_a,
                                       StorageLock(StorageLockType::kNone));
                  if (!lk_a) {
                    throw std::runtime_error("Failed to reserve lock");
                  }
                  net_storage_cost += lk_a->locked_tokens_;
                  co_await set.bg();
                  auto res = item.commit(insert_a, *lk_a);
                  net_storage_cost -= res;
                },
                item, net_storage_cost);

            // Reserve B insert(2)
            actions->add_action(
                [](model::RunnableActionSet &set, StorageItemDeltaManager &item,
                   int128_t &net_storage_cost) -> model::Async {
                  co_await set.bg();
                  auto insert_b =
                      make_rcq_delta(std::nullopt,
                                     RetrocausalQueueInsertDelta{
                                         .added_entries = {
                                             make_test_rcq_entry(2)}});
                  auto lk_b =
                      item.try_reserve(insert_b,
                                       StorageLock(StorageLockType::kNone));
                  if (!lk_b) {
                    throw std::runtime_error("Failed to reserve lock");
                  }
                  net_storage_cost += lk_b->locked_tokens_;
                  co_await set.bg();
                  auto res = item.commit(insert_b, *lk_b);
                  net_storage_cost -= res;
                },
                item, net_storage_cost);

            // Reserve C consume(1)
            actions->add_action(
                [](model::RunnableActionSet &set, StorageItemDeltaManager &item,
                   int128_t &net_storage_cost) -> model::Async {
                  co_await set.bg();
                  auto consume_c = make_rcq_delta(
                      RetrocausalQueueConsumeDelta{
                          .past_block_consume =
                              RetrocausalQueuePastBlockConsumeDelta{
                                  .max_fully_consumed_block_number = 0,
                                  .partial_consumed_number = 0},
                          .current_block_consumed = {make_test_rcq_entry(1)}},
                      RetrocausalQueueInsertDelta{.added_entries = {}});
                  std::optional<MeteredStorageLock> lk_c = std::nullopt;
                  while (!lk_c) {
                    lk_c =
                        item.try_reserve(consume_c,
                                         StorageLock(StorageLockType::kNone));
                    if (!lk_c) {
                      co_await set.bg();
                    }
                  }
                  net_storage_cost += lk_c->locked_tokens_;
                  co_await set.bg();
                  auto res = item.commit(consume_c, *lk_c);
                  net_storage_cost -= res;
                },
                item, net_storage_cost);
            return actions;
          },
          [](model::ActionResult res, StorageItemDeltaManager &item,
             int128_t &net_storage_cost) -> bool {
            if (res == model::ActionResult::kTimeout) {
              return item.mode() ==
                     BlockExecutionMode::kBlockExecutionModeProposal;
            }

            if (net_storage_cost !=
                cost_model.kBaseItemCost +
                    cost_model.get_retrocausal_queue_cost(1, 1)) {
              return false;
            }

            auto snapshot = item.get_post_close_block_item();
            if (!snapshot || snapshot->type() !=
                                 StorageItemType::kItemTypeRetrocausalQueue) {
              return false;
            }

            auto const &queue = snapshot->queue();
            return queue.get_active_stats() ==
                   RetrocausalQueueStats{.active_block_count = 1,
                                         .active_size = 1};
          });

  EXPECT_TRUE(pool.run_test(experiment));
}

namespace {
auto
make_bytestring_delta(Bytestring const &bytestring)
{
  StorageDelta delta(StorageDeltaType::kDeltaTypeBytestringSet);
  delta.bytestring() = bytestring;
  return delta;
}
} // namespace

TEST_P(DeltaManagerTest, BytestringCostUpgrade)
{
  InvalidStateFlag invalid_state_flag;

  // higher cost per byte and lower cost per item
  StorageCostModel cost_model_2(10000, 3, 1, 1, 5);
  Bytestring bytestring_1 = Bytestring{0, 1, 2, 3, 4};

  auto committed_item =
      std::make_shared<StorageItemWrapper>(bytestring_1, &cost_model_2);

  model::ThreadPool<StorageItemDeltaManager, int128_t> pool(4);
  std::shared_ptr<model::ExperimentBuilder<StorageItemDeltaManager, int128_t>>
      experiment = std::make_shared<
          model::ExperimentBuilder<StorageItemDeltaManager, int128_t>>(
          [&]() {
            return std::make_tuple(StorageItemDeltaManager(committed_item,
                                                           &invalid_state_flag,
                                                           mode_, &cost_model),
                                   int128_t{0});
          },
          [](model::WorkQueue &work_queue, StorageItemDeltaManager &item,
             int128_t &net_storage_cost) {
            auto actions =
                std::make_unique<model::RunnableActionSet>(work_queue);

            // Insert a bytestring of varying length
            actions->add_action(
                [](model::RunnableActionSet &set, StorageItemDeltaManager &item,
                   int128_t &net_storage_cost) -> model::Async {
                  co_await set.bg();

                  Bytestring bstring = Bytestring{0, 1};
                  if (set.choice(2)) {
                    bstring = Bytestring{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
                  }
                  auto delta = make_bytestring_delta(bstring);
                  auto lk =
                      item.try_reserve(delta,
                                       StorageLock(StorageLockType::kNone));
                  if (!lk) {
                    co_return;
                  }
                  net_storage_cost += lk->locked_tokens_;
                  co_await set.bg();
                  auto res = item.commit(delta, *lk);
                  net_storage_cost -= res;
                },
                item, net_storage_cost);

            // Insert a similarly varying length bytestring
            actions->add_action(
                [](model::RunnableActionSet &set, StorageItemDeltaManager &item,
                   int128_t &net_storage_cost) -> model::Async {
                  co_await set.bg();
                  Bytestring bstring = Bytestring{0, 1};
                  if (set.choice(2)) {
                    bstring = Bytestring{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
                  }
                  auto delta = make_bytestring_delta(bstring);
                  auto lk =
                      item.try_reserve(delta,
                                       StorageLock(StorageLockType::kNone));
                  if (!lk) {
                    co_return;
                  }
                  co_await set.bg();
                  auto delta2 =
                      make_bytestring_delta(Bytestring{0, 1, 2, 3, 4, 5, 6, 7});
                  // Possibly upgrade to an exclusive lock
                  auto lk2 = item.try_reserve(delta2, lk->lock_);
                  if (lk2) {
                    lk = lk2;
                    delta = delta2;
                  }
                  net_storage_cost += lk->locked_tokens_;
                  co_await set.bg();
                  auto res = item.commit(delta, *lk);
                  net_storage_cost -= res;
                },
                item, net_storage_cost);

            return actions;
          },
          [&cost_model_2](model::ActionResult res,
                          StorageItemDeltaManager &item,
                          int128_t &net_storage_cost) -> bool {
            if (res == model::ActionResult::kTimeout) {
              return false;
            }

            auto snapshot = item.get_post_close_block_item();
            if (!snapshot ||
                snapshot->type() != StorageItemType::kItemTypeBytestring) {
              return false;
            }

            auto const &bytestring = snapshot->bytestring();
            if (bytestring.size() != 10 && bytestring.size() != 2 &&
                bytestring.size() != 8) {
              return false;
            }

            return net_storage_cost ==
                   cost_model.kBaseItemCost +
                       cost_model.get_bytestring_cost(bytestring.size()) -
                       cost_model_2.kBaseItemCost -
                       cost_model_2.get_bytestring_cost(5);
          });

  EXPECT_TRUE(pool.run_test(experiment));
}

namespace {
model::Async
commit_delta_workflow(model::RunnableActionSet &set, StorageDelta delta,
                      StorageItemDeltaManager &item, int128_t &net_storage_cost,
                      bool must_succeed = false)
{
  co_await set.bg();
  auto lk = item.try_reserve(delta, StorageLock(StorageLockType::kNone));
  if (!lk) {
    if (must_succeed) {
      throw std::runtime_error("Failed to reserve lock");
    }
    co_return;
  }
  net_storage_cost += lk->locked_tokens_;
  co_await set.bg();
  auto res = item.commit(delta, *lk);
  net_storage_cost -= res;
  co_return;
}

} // namespace

TEST_P(DeltaManagerTest, TypeChangeCostUpgrade)
{
  // Test changes from a bytestring to an nnint
  InvalidStateFlag invalid_state_flag;

  // higher cost per item

  StorageCostModel cost_model_2(14000, 2, 1, 1, 5);
  Bytestring bytestring_1 = Bytestring{0, 1, 2, 3, 4};

  auto committed_item =
      std::make_shared<StorageItemWrapper>(bytestring_1, &cost_model_2);

  model::ThreadPool<StorageItemDeltaManager, int128_t> pool(4);
  std::shared_ptr<model::ExperimentBuilder<StorageItemDeltaManager, int128_t>>
      experiment = std::make_shared<
          model::ExperimentBuilder<StorageItemDeltaManager, int128_t>>(
          [&]() {
            return std::make_tuple(StorageItemDeltaManager(committed_item,
                                                           &invalid_state_flag,
                                                           mode_, &cost_model),
                                   int128_t{0});
          },
          [](model::WorkQueue &work_queue, StorageItemDeltaManager &item,
             int128_t &net_storage_cost) {
            auto actions =
                std::make_unique<model::RunnableActionSet>(work_queue);

            // do an nnint set_add
            actions->add_action(
                [](model::RunnableActionSet &set, StorageItemDeltaManager &item,
                   int128_t &net_storage_cost) -> model::Async {
                  return commit_delta_workflow(set, make_nnint_delta(10, 10),
                                               item, net_storage_cost, false);
                },
                item, net_storage_cost);

            // do an nnint set_add
            actions->add_action(
                [](model::RunnableActionSet &set, StorageItemDeltaManager &item,
                   int128_t &net_storage_cost) -> model::Async {
                  return commit_delta_workflow(set, make_nnint_delta(10, -5),
                                               item, net_storage_cost, false);
                },
                item, net_storage_cost);
            // do a delete
            actions->add_action(
                [](model::RunnableActionSet &set, StorageItemDeltaManager &item,
                   int128_t &net_storage_cost) -> model::Async {
                  return commit_delta_workflow(
                      set, StorageDelta(StorageDeltaType::kDeltaTypeDelete),
                      item, net_storage_cost, false);
                },
                item, net_storage_cost);

            return actions;
          },
          [&cost_model_2](model::ActionResult res,
                          StorageItemDeltaManager &item,
                          int128_t &net_storage_cost) -> bool {
            if (res == model::ActionResult::kTimeout) {
              return false;
            }

            // Either the delete commits or _both_ of the nnint set_adds commit.
            auto snapshot = item.get_post_close_block_item();
            if (snapshot &&
                snapshot->type() != StorageItemType::kItemTypeNonnegativeInt) {
              return false;
            }

            int128_t initial_cost = cost_model_2.kBaseItemCost +
                                    cost_model_2.get_bytestring_cost(5);
            if (!snapshot) {
              return initial_cost == -net_storage_cost;
            }
            if (snapshot->type() != StorageItemType::kItemTypeNonnegativeInt) {
              return false;
            }
            if (snapshot->nnint() != 15) {
              return false;
            }

            return net_storage_cost == cost_model.kBaseItemCost - initial_cost;
          });

  EXPECT_TRUE(pool.run_test(experiment));
}

TEST_P(DeltaManagerTest, NNIntAdd)
{
  // NNIntAdd should not conflict with NNIntSetAdd with the correct set base
  // value

  InvalidStateFlag invalid_state_flag;
  auto committed_item =
      std::make_shared<StorageItemWrapper>(int128_t{100}, &cost_model);

  model::ThreadPool<StorageItemDeltaManager, int128_t> pool(4);
  std::shared_ptr<model::ExperimentBuilder<StorageItemDeltaManager, int128_t>>
      experiment = std::make_shared<
          model::ExperimentBuilder<StorageItemDeltaManager, int128_t>>(
          [&]() {
            return std::make_tuple(StorageItemDeltaManager(committed_item,
                                                           &invalid_state_flag,
                                                           mode_, &cost_model),
                                   int128_t{0});
          },
          [](model::WorkQueue &work_queue, StorageItemDeltaManager &item,
             int128_t &net_storage_cost) {
            auto actions =
                std::make_unique<model::RunnableActionSet>(work_queue);

            actions->add_action(
                [](model::RunnableActionSet &set, StorageItemDeltaManager &item,
                   int128_t &net_storage_cost) -> model::Async {
                  return commit_delta_workflow(set, make_nnint_add_delta(10),
                                               item, net_storage_cost, true);
                },
                item, net_storage_cost);

            actions->add_action(
                [](model::RunnableActionSet &set, StorageItemDeltaManager &item,
                   int128_t &net_storage_cost) -> model::Async {
                  return commit_delta_workflow(set, make_nnint_add_delta(-50),
                                               item, net_storage_cost, true);
                },
                item, net_storage_cost);

            actions->add_action(
                [](model::RunnableActionSet &set, StorageItemDeltaManager &item,
                   int128_t &net_storage_cost) -> model::Async {
                  return commit_delta_workflow(set, make_nnint_delta(100, -10),
                                               item, net_storage_cost, true);
                },
                item, net_storage_cost);

            return actions;
          },
          [](model::ActionResult res, StorageItemDeltaManager &item,
             int128_t &net_storage_cost) -> bool {
            if (res == model::ActionResult::kTimeout) {
              return false;
            }

            auto snapshot = item.get_post_close_block_item();
            if (!snapshot ||
                snapshot->type() != StorageItemType::kItemTypeNonnegativeInt) {
              return false;
            }

            if (snapshot->nnint() != 100 - 50 - 10 + 10) {
              return false;
            }

            return net_storage_cost == 0;
          });

  EXPECT_TRUE(pool.run_test(experiment));
}

// NOLINTEND(bugprone-unchecked-optional-access)

TEST_P(DeltaManagerTest, AuthDataChangeSigner)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag, mode_,
                               &cost_model);

  // Create an auth data change delta for key ID 0
  StorageDelta auth_delta(StorageDeltaType::kDeltaTypeAuthDataChangeSigner);
  auth_delta.auth_data_change_signer().public_key_id = 0;
  auth_delta.auth_data_change_signer().public_key = 
      x23::Ptr<PublicKey>(new PublicKey(CryptographicScheme::Ed25519));

  // Try to reserve the delta
  auto lk1 = item.try_reserve(auth_delta, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk1);
  
  // Check that we can't reserve the same key ID again
  auto lk2 = item.try_reserve(auth_delta, StorageLock(StorageLockType::kNone));
  ASSERT_FALSE(lk2);

  // Commit the first change
  auto storage_excess = item.commit(auth_delta, *lk1);
  ASSERT_GE(storage_excess, 0);

  // Now try to reserve a different key ID
  StorageDelta auth_delta2(StorageDeltaType::kDeltaTypeAuthDataChangeSigner);
  auth_delta2.auth_data_change_signer().public_key_id = 1;
  auth_delta2.auth_data_change_signer().public_key = 
      x23::Ptr<PublicKey>(new PublicKey(CryptographicScheme::Ed25519));
  
  auto lk3 = item.try_reserve(auth_delta2, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk3);
  item.commit(auth_delta2, *lk3);

  // Check the final state
  auto final_item = item.get_post_close_block_item();
  ASSERT_TRUE(final_item);
  ASSERT_EQ(final_item->type(), StorageItemType::kItemTypeAuthData);
  
  // Verify the auth data has our changes
  const auto &auth_data = final_item->auth_data();
  ASSERT_TRUE(auth_data.public_keys[0]);
  ASSERT_TRUE(auth_data.public_keys[1]);
  ASSERT_FALSE(auth_data.public_keys[2]); // Should still be null
}

TEST_P(DeltaManagerTest, RCQFailedReservationReleasesReference)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag, mode_, &cost_model);

  // First, reserve RCQ with consume to set the consumer_lock_
  StorageDelta rcq_delta1(StorageDeltaType::kDeltaTypeRetrocausalQueue);
  rcq_delta1.retrocausal_queue().insert = RetrocausalQueueInsertDelta{};
  rcq_delta1.retrocausal_queue().consume.emplace(RetrocausalQueueConsumeDelta{
      .past_block_consume = RetrocausalQueuePastBlockConsumeDelta{
          .max_fully_consumed_block_number = 0,
          .partial_consumed_number = 0},
      .current_block_consumed = {}
  });

  auto lk1 = item.try_reserve(rcq_delta1, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk1); // Should succeed

  // Now try to reserve another RCQ delta that also needs consume
  // This should fail because consumer_lock_ is already true
  StorageDelta rcq_delta2(StorageDeltaType::kDeltaTypeRetrocausalQueue);
  rcq_delta2.retrocausal_queue().insert = RetrocausalQueueInsertDelta{};
  rcq_delta2.retrocausal_queue().consume.emplace(RetrocausalQueueConsumeDelta{
      .past_block_consume = RetrocausalQueuePastBlockConsumeDelta{
          .max_fully_consumed_block_number = 0,
          .partial_consumed_number = 0},
      .current_block_consumed = {}
  });

  auto lk2 = item.try_reserve(rcq_delta2, StorageLock(StorageLockType::kNone));
  ASSERT_FALSE(lk2); // Should fail due to consumer_lock_ conflict

  // Clean up the first reservation
  item.revert(lk1->lock_);

  // Now try to reserve a completely different type (bytestring)
  // This should succeed if the reference was properly released
  StorageDelta bytestring_delta(StorageDeltaType::kDeltaTypeBytestringSet);
  bytestring_delta.bytestring() = {1, 2, 3, 4, 5};
  
  auto lk3 = item.try_reserve(bytestring_delta, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk3); // Should succeed if reference was released

  // Clean up
  item.commit(bytestring_delta, *lk3);
}

TEST_P(DeltaManagerTest, AuthDataFailedReservationReleasesReference)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag, mode_, &cost_model);

  // First, create a successful auth data reservation to occupy key ID 0
  StorageDelta auth_delta1(StorageDeltaType::kDeltaTypeAuthDataChangeSigner);
  auth_delta1.auth_data_change_signer().public_key_id = 0;
  auth_delta1.auth_data_change_signer().public_key = 
      x23::Ptr<PublicKey>(new PublicKey(CryptographicScheme::Ed25519));

  auto lk1 = item.try_reserve(auth_delta1, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk1);

  // Now try to reserve the same key ID again - this should fail
  StorageDelta auth_delta2(StorageDeltaType::kDeltaTypeAuthDataChangeSigner);
  auth_delta2.auth_data_change_signer().public_key_id = 0; // Same key ID
  auth_delta2.auth_data_change_signer().public_key = 
      x23::Ptr<PublicKey>(new PublicKey(CryptographicScheme::Ed25519));

  auto lk2 = item.try_reserve(auth_delta2, StorageLock(StorageLockType::kNone));
  ASSERT_FALSE(lk2); // Should fail due to conflict

  // revert the first change to clean up
  item.revert(lk1->lock_);

  // Now try to reserve a completely different type (bytestring)
  // This should succeed if the reference was properly released
  StorageDelta bytestring_delta(StorageDeltaType::kDeltaTypeBytestringSet);
  bytestring_delta.bytestring() = {1, 2, 3, 4, 5};
  
  auto lk3 = item.try_reserve(bytestring_delta, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lk3); // Should succeed if reference was released

  // Clean up
  item.commit(bytestring_delta, *lk3);
}

INSTANTIATE_TEST_SUITE_P(
    BlockModes, DeltaManagerTest,
    ::testing::Values(BlockExecutionMode::kBlockExecutionModeProposal,
                      BlockExecutionMode::kBlockExecutionModeValidation),
    [](const ::testing::TestParamInfo<BlockExecutionMode> &info)
        -> std::string {
      switch (info.param) {
      case BlockExecutionMode::kBlockExecutionModeProposal:
        return "Proposal";
      case BlockExecutionMode::kBlockExecutionModeValidation:
        return "Validation";
      default:
        return x23::show(info.param);
      }
    });

} // namespace hogd
