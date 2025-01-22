#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogdb/softstate/storage_item_frame.h"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

// Change the typeclass of a storage item during execution
TEST(StorageItemFrame, RewriteVariantTypeNullStart)
{
  StorageItemFrame item(AddressAndKey{}, std::nullopt);

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
  item.revert(*b_lock);

  // item in neutral state
  auto nnint_lock_1 =
      item.try_reserve(nnint_set_add_1, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(nnint_lock_1);
  // mismatch in nnint set value
  EXPECT_FALSE(
      item.try_reserve(nnint_set_add_2, StorageLock(StorageLockType::kNone)));
  item.revert(*nnint_lock_1);

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
  item.revert(*nnint_lock_3);

  nnint_lock_3 =
      item.try_reserve(nnint_set_add_3, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(nnint_lock_3);

  item.revert(*nnint_lock_3);
  item.revert(*nnint_lock_2);
  // item in neutral state

  EXPECT_TRUE(
      item.try_reserve(bytestring_delta, StorageLock(StorageLockType::kNone)));
}

TEST(StorageItemFrame, RewriteVariantFromBase)
{
  StorageItem base_item;

  base_item.type(StorageItemType::kBytestring);
  base_item.bytestring() = {0, 1, 2, 3};

  StorageItemFrame item(AddressAndKey{}, base_item);

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
  item.revert(*nnint_lock);

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

  StorageItemFrame item2(AddressAndKey{}, item.get_post_close_block_item());

  auto committed_item = item2.get_snapshot_item();

  ASSERT_TRUE(committed_item.has_value());

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  EXPECT_EQ(committed_item->type(), StorageItemType::kBytestring);

  EXPECT_EQ(committed_item->bytestring(), bytestring_delta.bytestring());
  // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST(StorageItemFrame, DeleteItem)
{
  StorageItem base_item;

  base_item.type(StorageItemType::kBytestring);
  base_item.bytestring() = {0, 1, 2, 3};

  StorageItemFrame item(AddressAndKey{}, base_item);
  ASSERT_TRUE(item.get_snapshot_item());

  // committing an empty round keeps item active
  StorageItemFrame item2(AddressAndKey{}, item.get_post_close_block_item());

  ASSERT_TRUE(item2.get_snapshot_item());

  auto delete_lock = item2.try_reserve(StorageDelta(StorageDeltaType::kDelete),
                                       StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(delete_lock);

  StorageDelta bytestring_delta(StorageDeltaType::kBytestringSet);
  bytestring_delta.bytestring() = {3, 2, 1, 0};
  EXPECT_FALSE(
      item2.try_reserve(bytestring_delta, StorageLock(StorageLockType::kNone)));

  item2.commit(StorageDelta(StorageDeltaType::kDelete), *delete_lock);

  StorageItemFrame item3(AddressAndKey{}, item2.get_post_close_block_item());

  EXPECT_FALSE(item3.get_snapshot_item());
}

TEST(StorageItemFrame, ExclusiveLockAcquire)
{
  StorageItemFrame item(AddressAndKey{}, std::nullopt);

  auto lock1 = item.try_reserve(StorageDelta(StorageDeltaType::kDelete),
                                StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lock1);

  StorageDelta bytestring_delta(StorageDeltaType::kBytestringSet);
  bytestring_delta.bytestring() = {3, 2, 1, 0};

  auto lock2 = item.try_reserve(bytestring_delta, *lock1);
  ASSERT_TRUE(lock2);

  EXPECT_EQ(lock2->lock_type(), StorageLockType::kExclusiveLock);

  item.commit(bytestring_delta, *lock2);

  auto snapshot = item.get_post_close_block_item();

  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->type(), StorageItemType::kBytestring);
  EXPECT_EQ(snapshot->bytestring(), bytestring_delta.bytestring());
}

TEST(StorageItemFrame, NNIntSharedLock2SubSecondFails)
{
  StorageItemFrame item(AddressAndKey{}, std::nullopt);

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

  auto lock2 = item.try_reserve(nnint_set_add_2, *lock1);
  ASSERT_TRUE(lock2);

  auto lock3 = item.try_reserve(nnint_set_add_3, *lock2);
  ASSERT_TRUE(lock3);

  ASSERT_EQ(*lock3, *lock2);

  ASSERT_EQ(lock3->lock_type(), StorageLockType::kSetItemTypeClass);

  EXPECT_EQ(lock3->full_typeclass_lock().typeclass.item_type(),
            StorageItemTypeClass::kNonnegativeInt);
  EXPECT_EQ(
      lock3->full_typeclass_lock().extra_lock_data.nnint_subtracted_amount(),
      to_xdr(int128_t{-4}));

  auto lock4 = item.try_reserve(nnint_set_add_4, *lock3);
  ASSERT_TRUE(lock4);

  EXPECT_EQ(lock4->lock_type(), StorageLockType::kSetItemTypeClass);
  EXPECT_EQ(lock4->full_typeclass_lock().typeclass.item_type(),
            StorageItemTypeClass::kNonnegativeInt);
  EXPECT_EQ(
      lock4->full_typeclass_lock().extra_lock_data.nnint_subtracted_amount(),
      to_xdr(int128_t{-5}));

  // return to nnint_set_add_2
  auto final_lock = item.try_reserve(nnint_set_add_2, *lock4);
  ASSERT_TRUE(final_lock);

  item.commit(nnint_set_add_2, *final_lock);

  auto snapshot = item.get_post_close_block_item();
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->type(), StorageItemType::kNonnegativeInt);
  EXPECT_EQ(snapshot->nnint(), to_xdr(int128_t{1}));
}

TEST(StorageItemFrame, NNIntFromExclusiveLock)
{
  StorageItemFrame item(AddressAndKey{}, std::nullopt);

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

  auto lock2 = item.try_reserve(nnint_set_add_2, *lock1);
  ASSERT_TRUE(lock2);

  EXPECT_EQ(lock2->lock_type(), StorageLockType::kExclusiveLock);

  // check to assert only one exclusive lock can be given out.
  EXPECT_FALSE(item.try_acquire_exclusive_lock(StorageLockType::kNone));
  item.commit(nnint_set_add_2, *lock2);
  EXPECT_FALSE(item.try_acquire_exclusive_lock(StorageLockType::kNone));

  auto snapshot = item.get_post_close_block_item();
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->type(), StorageItemType::kNonnegativeInt);
  EXPECT_EQ(snapshot->nnint(), to_xdr(int128_t{6}));
}

} // namespace hogd
