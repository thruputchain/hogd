#include <gtest/gtest.h>

#include <optional>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/db_interface/transaction_state.h"
#include "hogd/storage_item/storage_lock_util.h"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "types.hh"

namespace hogd {
namespace {

StorageDelta
make_bstring_delta(Bytestring b)
{
  StorageDelta delta(StorageDeltaType::kDeltaTypeBytestringSet);
  delta.bytestring() = std::move(b);
  return delta;
}

MeteredStorageLock
make_bstring_lock(Bytestring b)
{
  StorageLock lock(StorageLockType::kSetItemTypeClass);
  auto &typeclass = lock.full_typeclass_lock().typeclass;
  typeclass.item_type(StorageItemType::kItemTypeBytestring);
  typeclass.bytestring() = std::move(b);
  lock.full_typeclass_lock().extra_lock_data.item_type(
      StorageItemType::kItemTypeBytestring);
  return MeteredStorageLock(std::move(lock), 1);
}

MeteredStorageLock
make_exclusive_lock()
{
  StorageLock lock(StorageLockType::kExclusiveLock);
  return MeteredStorageLock(std::move(lock), 1);
}

AddressAndKey
make_addrkey(uint8_t i)
{
  AddressAndKey addrkey{};
  addrkey.address[0] = i;
  return addrkey;
}

} // namespace

TEST(TransactionState, EmptyTest)
{
  TransactionState tx;
  tx.push_subtransaction();
  tx.add_delta(make_addrkey(0), make_bstring_delta({1, 2, 3}),
               make_bstring_lock({1, 2, 3}));
  tx.pop_subtransactions(1);

  auto results = tx.extract_results();
  ASSERT_EQ(results.size(), 1);
  // we have a lock, but no delta
  EXPECT_EQ(results.at(make_addrkey(0)).first, std::nullopt);
  EXPECT_EQ(results.at(make_addrkey(0)).second.lock_.lock_type(),
            StorageLockType::kSetItemTypeClass);
}

TEST(TransactionState, SubtransactionTest)
{
  TransactionState tx;
  tx.push_subtransaction();

  tx.add_delta(make_addrkey(0), make_bstring_delta({1, 2, 3}),
               make_bstring_lock({1, 2, 3}));
  tx.push_subtransaction();
  tx.add_delta(make_addrkey(0), make_bstring_delta({7, 8, 9}),
               make_exclusive_lock());
  // delta that gets reverted -- assert that we don't forget that we have the
  // lock
  tx.add_delta(make_addrkey(1), make_bstring_delta({4, 5, 6}),
               make_bstring_lock({4, 5, 6}));
  tx.pop_subtransactions(1);

  auto results = tx.extract_results();

  ASSERT_EQ(results.size(), 2);
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_TRUE(results.at(make_addrkey(0)).first.has_value());
  EXPECT_EQ(results.at(make_addrkey(0)).first->bytestring(),
            Bytestring({1, 2, 3}));
  EXPECT_EQ(results.at(make_addrkey(0)).second.lock_.lock_type(),
            StorageLockType::kExclusiveLock);
  EXPECT_EQ(results.at(make_addrkey(1)).first, std::nullopt);
  // NOLINTEND(bugprone-unchecked-optional-access)
}

} // namespace hogd
