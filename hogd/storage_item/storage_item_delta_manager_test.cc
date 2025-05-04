#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/storage_item/invalid_state_flag.h"
#include "hogd/storage_item/storage_costs.h"
#include "hogd/storage_item/storage_item_delta_manager.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

// ASSERT_TRUE(x) doesn't show up to clangd as a check for the optional
// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace {
void
expect_final(StorageItemDeltaManager &item, Bytestring const &bytestring)
{
  auto snapshot = item.get_post_close_block_item();
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->type(), StorageItemType::kBytestring);
  EXPECT_EQ(snapshot->bytestring(), bytestring);
}
void
expect_final(StorageItemDeltaManager &item, int128_t nnint)
{
  auto snapshot = item.get_post_close_block_item();
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->type(), StorageItemType::kNonnegativeInt);
  EXPECT_EQ(snapshot->nnint(), nnint);
}

} // namespace

// Change the typeclass of a storage item during execution
TEST(StorageItemFrame, RewriteVariantTypeNullStart)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag);

  auto make_nnint_delta = [](int128_t set, int128_t add) {
    StorageDelta delta(StorageDeltaType::kNonnegativeIntSetAdd);
    delta.nnint().set_value = to_xdr(set);
    delta.nnint().add_value = to_xdr(add);
    return delta;
  };

  StorageDelta bytestring_delta(StorageDeltaType::kBytestringSet);
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
  // fails because too much subtraction
  EXPECT_FALSE(
      item.try_reserve(nnint_set_add_3, StorageLock(StorageLockType::kNone)));
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

TEST(StorageItemFrame, RewriteVariantFromBase)
{
  auto base_item = std::make_shared<StorageItemWrapper>(Bytestring{0, 1, 2, 3});
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(base_item, &invalid_state_flag);

  StorageDelta bytestring_delta(StorageDeltaType::kBytestringSet);
  bytestring_delta.bytestring() = {3, 2, 1, 0};

  auto make_nnint_delta = [](int128_t set, int128_t add) {
    StorageDelta delta(StorageDeltaType::kNonnegativeIntSetAdd);
    delta.nnint().set_value = to_xdr(set);
    delta.nnint().add_value = to_xdr(add);
    return delta;
  };

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
                                &invalid_state_flag);
  expect_final(item, bytestring_delta.bytestring());
  expect_final(item2, bytestring_delta.bytestring());
  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST(StorageItemFrame, DeleteItem)
{
  InvalidStateFlag invalid_state_flag;
  auto base_item = std::make_shared<StorageItemWrapper>(Bytestring{0, 1, 2, 3});

  StorageItemDeltaManager item(base_item, &invalid_state_flag);
  ASSERT_TRUE(item.get_snapshot_item());

  // committing an empty round keeps item active
  StorageItemDeltaManager item2(item.get_post_close_block_item(),
                                &invalid_state_flag);

  ASSERT_TRUE(item2.get_snapshot_item());

  auto delete_lock = item2.try_reserve(StorageDelta(StorageDeltaType::kDelete),
                                       StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(delete_lock);

  StorageDelta bytestring_delta(StorageDeltaType::kBytestringSet);
  bytestring_delta.bytestring() = {3, 2, 1, 0};
  EXPECT_FALSE(
      item2.try_reserve(bytestring_delta, StorageLock(StorageLockType::kNone)));

  item2.commit(StorageDelta(StorageDeltaType::kDelete), *delete_lock);
  ASSERT_FALSE(invalid_state_flag.is_invalid());
  ASSERT_FALSE(item2.get_post_close_block_item()); // item2 has the item deleted
                                                   // post block

  StorageItemDeltaManager item3(item2.get_post_close_block_item(),
                                &invalid_state_flag);

  EXPECT_FALSE(item3.get_snapshot_item());
  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST(StorageItemFrame, ExclusiveLockAcquire)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag);

  auto lock1 = item.try_reserve(StorageDelta(StorageDeltaType::kDelete),
                                StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lock1);

  StorageDelta bytestring_delta(StorageDeltaType::kBytestringSet);
  bytestring_delta.bytestring() = {3, 2, 1, 0};

  auto lock2 = item.try_reserve(bytestring_delta, lock1->lock_);
  ASSERT_TRUE(lock2);
  EXPECT_EQ(lock2->lock_.lock_type(), StorageLockType::kExclusiveLock);

  item.commit(bytestring_delta, *lock2);

  expect_final(item, bytestring_delta.bytestring());
  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST(StorageItemFrame, NNIntSharedLock2SubSecondFails)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag);

  auto make_nnint_delta = [](int128_t set, int128_t add) {
    StorageDelta delta(StorageDeltaType::kNonnegativeIntSetAdd);
    delta.nnint().set_value = to_xdr(set);
    delta.nnint().add_value = to_xdr(add);
    return delta;
  };

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
            StorageItemTypeClass::kNonnegativeInt);
  EXPECT_EQ(lock3->lock_.full_typeclass_lock()
                .extra_lock_data.nnint_subtracted_amount(),
            to_xdr(int128_t{-4}));

  auto lock4 = item.try_reserve(nnint_set_add_4, lock3->lock_);
  ASSERT_TRUE(lock4);

  EXPECT_EQ(lock4->lock_.lock_type(), StorageLockType::kSetItemTypeClass);
  EXPECT_EQ(lock4->lock_.full_typeclass_lock().typeclass.item_type(),
            StorageItemTypeClass::kNonnegativeInt);
  EXPECT_EQ(lock4->lock_.full_typeclass_lock()
                .extra_lock_data.nnint_subtracted_amount(),
            to_xdr(int128_t{-5}));

  // return to nnint_set_add_2
  auto final_lock = item.try_reserve(nnint_set_add_2, lock4->lock_);
  ASSERT_TRUE(final_lock);

  item.commit(nnint_set_add_2, *final_lock);

  expect_final(item, int128_t{1});
  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST(StorageItemFrame, NNIntFromExclusiveLock)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag);

  auto make_nnint_delta = [](int128_t set, int128_t add) {
    StorageDelta delta(StorageDeltaType::kNonnegativeIntSetAdd);
    delta.nnint().set_value = to_xdr(set);
    delta.nnint().add_value = to_xdr(add);
    return delta;
  };

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

TEST(StorageItemFrame, ExclusiveBytestring)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag);

  StorageDelta bytestring_delta(StorageDeltaType::kBytestringSet);
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

TEST(StorageItemFrame, ExclusiveLockMultiAcquire)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag);

  // This is just a placeholder for the test
  StorageDelta bytestring_delta(StorageDeltaType::kBytestringSet);
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

  item.commit(StorageDelta(StorageDeltaType::kDelete), *lock);

  ASSERT_FALSE(
      item.try_acquire_exclusive_lock(bytestring_delta, StorageLockType::kNone,
                                      StorageLockType::kExclusiveLock));
  ASSERT_FALSE(
      item.try_reserve(StorageDelta(StorageDeltaType::kNonnegativeIntSetAdd),
                       StorageLock(StorageLockType::kNone)));

  // exclusive lock was committed but released, so we can acquire shared lock.
  // Was not written in permanent exclusive mode.
  ASSERT_TRUE(item.try_reserve(StorageDelta(StorageDeltaType::kDelete),
                               StorageLock(StorageLockType::kNone)));

  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST(StorageItemFrame, PermanentExclusiveLockMultiAcquire)
{
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag);

  // merely a placeholder for the test
  StorageDelta bytestring_delta(StorageDeltaType::kBytestringSet);
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

  item.commit(StorageDelta(StorageDeltaType::kDelete), *lock);

  ASSERT_FALSE(
      item.try_acquire_exclusive_lock(bytestring_delta, StorageLockType::kNone,
                                      StorageLockType::kExclusiveLock));
  ASSERT_FALSE(
      item.try_reserve(StorageDelta(StorageDeltaType::kNonnegativeIntSetAdd),
                       StorageLock(StorageLockType::kNone)));

  // cannot reacquire lock after permament lock commits
  ASSERT_FALSE(item.try_reserve(StorageDelta(StorageDeltaType::kDelete),
                                StorageLock(StorageLockType::kNone)));
  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST(StorageItemFrame, NNIntSimpleSetAdd)
{
  auto make_nnint_delta = [](int128_t set, int128_t add) {
    StorageDelta delta(StorageDeltaType::kNonnegativeIntSetAdd);
    delta.nnint().set_value = to_xdr(set);
    delta.nnint().add_value = to_xdr(add);
    return delta;
  };
  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag);

  StorageDelta nnint_set_add = make_nnint_delta(5, 10);

  auto lock =
      item.try_reserve(nnint_set_add, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lock);

  item.commit(nnint_set_add, *lock);

  expect_final(item, int128_t{15});
  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST(StorageItemCosts, ExclusiveLockCostMatchNoPriorCommit)
{
  auto make_bytestring_delta = [](x23::Vec<uint8_t> vec) {
    StorageDelta delta(StorageDeltaType::kBytestringSet);
    delta.bytestring() = std::move(vec);
    return delta;
  };

  InvalidStateFlag invalid_state_flag;
  StorageItemDeltaManager item(nullptr, &invalid_state_flag);

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
            lk_exclusive->locked_tokens_ + bytestring_cost(2));
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
