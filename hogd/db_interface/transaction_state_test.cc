#include <gtest/gtest.h>

#include <utility>

#include "hogd/db_interface/transaction_state.h"
#include "hogd/storage_item/storage_lock_util.h"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "types.hh"


namespace hogd {
namespace {

StorageDelta
make_bstring_delta(Bytestring b) {
    StorageDelta delta(StorageDeltaType::kDeltaTypeBytestringSet);
    delta.bytestring() = std::move(b);   
    return delta;
}

MeteredStorageLock
make_bstring_lock(Bytestring b) {
    StorageLock lock(StorageLockType::kSetItemTypeClass);
    auto& typeclass = lock.full_typeclass_lock().typeclass;
    typeclass.item_type(StorageItemType::kItemTypeBytestring);
    typeclass.bytestring() = std::move(b);
    lock.full_typeclass_lock().extra_lock_data.item_type(StorageItemType::kItemTypeBytestring);
    return MeteredStorageLock(std::move(lock), 1);
}

MeteredStorageLock
make_exclusive_lock() {
    StorageLock lock(StorageLockType::kExclusiveLock);
    return MeteredStorageLock(std::move(lock), 1);
}

}

TEST(TransactionState, SubtransactionTest) {
    TransactionState tx;

    tx.add_delta(AddressAndKey{}, make_bstring_delta({1,2,3}), make_bstring_lock({1,2,3}));
    tx.push_subtransaction();
    tx.add_delta(AddressAndKey{}, make_bstring_delta({4,5,6}), make_exclusive_lock());
    tx.pop_subtransactions(1);

    auto results = tx.extract_results();

    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results[AddressAndKey{}].first.bytestring(), Bytestring({1,2,3}));
    EXPECT_EQ(results[AddressAndKey{}].second.lock_.lock_type(), StorageLockType::kExclusiveLock);
}

} // namespace hogd