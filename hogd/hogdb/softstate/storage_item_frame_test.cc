#include <atomic>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogdb/softstate/storage_item_frame.h"
#include "hogd/hogdb/storage_item/storage_item_wrapper.h"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "types.hh"

namespace hogd {

// ASSERT_TRUE(x) doesn't show up to clangd as a check for the optional
// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace {
void
expect_final(StorageItemFrame &item, Bytestring const &bytestring)
{
  auto snapshot = item.get_post_close_block_item();
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->type(), StorageItemType::kBytestring);
  EXPECT_EQ(snapshot->bytestring(), bytestring);
}
void
expect_final(StorageItemFrame &item, int128_t nnint)
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
  std::atomic<uint32_t> counter = 0;
  StorageItemFrame item(AddressAndKey{}, std::nullopt, &counter);

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

  ASSERT_EQ(counter, 0);
}

TEST(StorageItemFrame, RewriteVariantFromBase)
{
  std::atomic<uint32_t> counter = 0;
  StorageItemWrapper base_item({0, 1, 2, 3});

  StorageItemFrame item(AddressAndKey{}, base_item, &counter);

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

  StorageItemFrame item2(AddressAndKey{}, item.get_post_close_block_item(),
                         &counter);
  expect_final(item, bytestring_delta.bytestring());
  expect_final(item2, bytestring_delta.bytestring());

  ASSERT_EQ(counter, 0);
}

TEST(StorageItemFrame, DeleteItem)
{
  std::atomic<uint32_t> bad_item_count = 0;
  StorageItemWrapper base_item({0, 1, 2, 3});

  StorageItemFrame item(AddressAndKey{}, base_item, &bad_item_count);
  ASSERT_TRUE(item.get_snapshot_item());

  // committing an empty round keeps item active
  StorageItemFrame item2(AddressAndKey{}, item.get_post_close_block_item(),
                         &bad_item_count);

  ASSERT_TRUE(item2.get_snapshot_item());

  auto delete_lock = item2.try_reserve(StorageDelta(StorageDeltaType::kDelete),
                                       StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(delete_lock);

  StorageDelta bytestring_delta(StorageDeltaType::kBytestringSet);
  bytestring_delta.bytestring() = {3, 2, 1, 0};
  EXPECT_FALSE(
      item2.try_reserve(bytestring_delta, StorageLock(StorageLockType::kNone)));

  item2.commit(StorageDelta(StorageDeltaType::kDelete), *delete_lock);

  StorageItemFrame item3(AddressAndKey{}, item2.get_post_close_block_item(),
                         &bad_item_count);

  EXPECT_FALSE(item3.get_snapshot_item());

  ASSERT_EQ(bad_item_count, 0);
}

TEST(StorageItemFrame, ExclusiveLockAcquire)
{
  std::atomic<uint32_t> bad_item_count = 0;
  StorageItemFrame item(AddressAndKey{}, std::nullopt, &bad_item_count);

  auto lock1 = item.try_reserve(StorageDelta(StorageDeltaType::kDelete),
                                StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lock1);

  StorageDelta bytestring_delta(StorageDeltaType::kBytestringSet);
  bytestring_delta.bytestring() = {3, 2, 1, 0};

  auto lock2 = item.try_reserve(bytestring_delta, *lock1);
  ASSERT_TRUE(lock2);

  EXPECT_EQ(lock2->lock_type(), StorageLockType::kExclusiveLock);

  item.commit(bytestring_delta, *lock2);

  expect_final(item, bytestring_delta.bytestring());
  ASSERT_EQ(bad_item_count, 0);
}

TEST(StorageItemFrame, NNIntSharedLock2SubSecondFails)
{
  std::atomic<uint32_t> bad_item_count = 0;
  StorageItemFrame item(AddressAndKey{}, std::nullopt, &bad_item_count);

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

  expect_final(item, int128_t{1});
  ASSERT_EQ(bad_item_count, 0);
}

TEST(StorageItemFrame, NNIntFromExclusiveLock)
{
  std::atomic<uint32_t> bad_item_count = 0;
  StorageItemFrame item(AddressAndKey{}, std::nullopt, &bad_item_count);

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

  expect_final(item, int128_t{6});
  ASSERT_EQ(bad_item_count, 0);
}

TEST(StorageItemFrame, ExclusiveBytestring)
{
  std::atomic<uint32_t> bad_item_count = 0;
  StorageItemFrame item(AddressAndKey{}, std::nullopt, &bad_item_count);

  StorageDelta bytestring_delta(StorageDeltaType::kBytestringSet);
  bytestring_delta.bytestring() = {3, 2, 1, 0};

  auto res = item.try_acquire_exclusive_lock(StorageLockType::kNone);
  ASSERT_TRUE(res);

  auto lock =
      item.try_reserve(bytestring_delta,
                       StorageLock(StorageLockType::kPermanentExclusiveLock));
  ASSERT_TRUE(lock);

  item.commit(bytestring_delta, *lock);

  ASSERT_FALSE(item.try_acquire_exclusive_lock(StorageLockType::kNone));
  ASSERT_FALSE(
      item.try_reserve(bytestring_delta, StorageLock(StorageLockType::kNone)));

  expect_final(item, bytestring_delta.bytestring());
  ASSERT_EQ(bad_item_count, 0);
}

TEST(StorageItemFrame, ExclusiveLockMultiAcquire)
{
  std::atomic<uint32_t> bad_item_count = 0;
  StorageItemFrame item(AddressAndKey{}, std::nullopt, &bad_item_count);

  ASSERT_TRUE(item.try_acquire_exclusive_lock(StorageLockType::kNone));

  ASSERT_FALSE(item.try_acquire_exclusive_lock(StorageLockType::kNone));
  // Should never give out an exclusive lock and a SetItemTypeClass lock.
  ASSERT_TRUE(item.try_acquire_exclusive_lock(StorageLockType::kExclusiveLock));
  ASSERT_TRUE(item.try_acquire_exclusive_lock(
      StorageLockType::kPermanentExclusiveLock));

  item.commit(StorageDelta(StorageDeltaType::kDelete),
              StorageLock(StorageLockType::kExclusiveLock));

  ASSERT_FALSE(item.try_acquire_exclusive_lock(StorageLockType::kNone));
  ASSERT_FALSE(
      item.try_reserve(StorageDelta(StorageDeltaType::kNonnegativeIntSetAdd),
                       StorageLock(StorageLockType::kNone)));

  // exclusive lock was committed but released, so we can acquire shared lock.
  // Was not written in permanent exclusive mode.
  ASSERT_TRUE(item.try_reserve(StorageDelta(StorageDeltaType::kDelete),
                               StorageLock(StorageLockType::kNone)));

  ASSERT_EQ(bad_item_count, 0);
}

TEST(StorageItemFrame, PermanentExclusiveLockMultiAcquire)
{
  std::atomic<uint32_t> bad_item_count = 0;
  StorageItemFrame item(AddressAndKey{}, std::nullopt, &bad_item_count);

  ASSERT_TRUE(item.try_acquire_exclusive_lock(StorageLockType::kNone));

  ASSERT_FALSE(item.try_acquire_exclusive_lock(StorageLockType::kNone));
  // Should never give out an exclusive lock and a SetItemTypeClass lock.
  ASSERT_TRUE(item.try_acquire_exclusive_lock(
      StorageLockType::kPermanentExclusiveLock));

  item.commit(StorageDelta(StorageDeltaType::kDelete),
              StorageLock(StorageLockType::kPermanentExclusiveLock));

  ASSERT_FALSE(item.try_acquire_exclusive_lock(StorageLockType::kNone));
  ASSERT_FALSE(
      item.try_reserve(StorageDelta(StorageDeltaType::kNonnegativeIntSetAdd),
                       StorageLock(StorageLockType::kNone)));

  // cannot reacquire lock after permament lock commits
  ASSERT_FALSE(item.try_reserve(StorageDelta(StorageDeltaType::kDelete),
                                StorageLock(StorageLockType::kNone)));
  ASSERT_EQ(bad_item_count, 0);
}

TEST(StorageItemFrame, NNIntSimpleSetAdd)
{
  auto make_nnint_delta = [](int128_t set, int128_t add) {
    StorageDelta delta(StorageDeltaType::kNonnegativeIntSetAdd);
    delta.nnint().set_value = to_xdr(set);
    delta.nnint().add_value = to_xdr(add);
    return delta;
  };
  std::atomic<uint32_t> bad_item_count = 0;
  StorageItemFrame item(AddressAndKey{}, std::nullopt, &bad_item_count);

  StorageDelta nnint_set_add = make_nnint_delta(5, 10);

  auto lock =
      item.try_reserve(nnint_set_add, StorageLock(StorageLockType::kNone));
  ASSERT_TRUE(lock);

  item.commit(nnint_set_add, *lock);

  expect_final(item, int128_t{15});
  ASSERT_EQ(bad_item_count, 0);
}
// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
