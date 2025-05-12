#include "hogd/storage_item/storage_lock_util.h"

#include <algorithm>
#include <stdexcept>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "storage_item.hh"
#include "storage_lock.hh"

namespace hogd {

StorageItemTypeClass
delta_to_typeclass(StorageDelta const &delta)
{
  switch (delta.type()) {
  case StorageDeltaType::kDeltaTypeBytestringSet:
  {
    auto out = StorageItemTypeClass(StorageItemType::kItemTypeBytestring);
    out.bytestring() = delta.bytestring();
    return out;
  }
  case StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd:
  {
    auto out = StorageItemTypeClass(StorageItemType::kItemTypeNonnegativeInt);
    out.nnint_set() = delta.nnint().set_value;
    return out;
  }
  case StorageDeltaType::kDeltaTypeDelete:
    return StorageItemTypeClass(StorageItemType::kItemTypeDeleted);
  case StorageDeltaType::kDeltaTypeRetrocausalQueue:
    return StorageItemTypeClass(StorageItemType::kItemTypeRetrocausalQueue);
  case StorageDeltaType::kDeltaTypeAssetTransfer:
    return StorageItemTypeClass(StorageItemType::kItemTypeAssetBalance);
  default:
    throw std::runtime_error("unimplemented");
  }
  // unreachable
}

StorageLock
delta_to_lock(StorageDelta const &delta)
{
  StorageLock lock(StorageLockType::kSetItemTypeClass);
  lock.full_typeclass_lock().typeclass = delta_to_typeclass(delta);
  switch (delta.type()) {
  case StorageDeltaType::kDeltaTypeBytestringSet:
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kItemTypeBytestring);
    return lock;
  case StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd:
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kItemTypeNonnegativeInt);
    lock.full_typeclass_lock().extra_lock_data.nnint_subtracted_amount() =
        to_xdr(std::min<int128_t>(0, from_xdr(delta.nnint().add_value)));
    return lock;
  case StorageDeltaType::kDeltaTypeRetrocausalQueue:
  {
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kItemTypeRetrocausalQueue);
    if (delta.retrocausal_queue().consume) {
      lock.full_typeclass_lock().extra_lock_data.consume_lock() = 1;
    }
    return lock;
  }
  case StorageDeltaType::kDeltaTypeDelete:
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kItemTypeDeleted);
    return lock;
  case StorageDeltaType::kDeltaTypeAssetTransfer:
  {
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kItemTypeAssetBalance);
    auto xfer = from_xdr(delta.transfer_amount());
    if (xfer > 0) {
      lock.full_typeclass_lock().extra_lock_data.asset_balance_lock() = AssetBalanceLock{
        .asset_add_amount = to_xdr(xfer),
        .asset_subtract_amount = {.highbits = 0, .lowbits = 0},
      };
    } else {
      lock.full_typeclass_lock().extra_lock_data.asset_balance_lock() = AssetBalanceLock{
        .asset_add_amount = {.highbits = 0, .lowbits = 0},
        .asset_subtract_amount = to_xdr(-xfer),
      };
    }
    return lock;
  }
  default:
    throw std::runtime_error("unimplemented");
  }
}

} // namespace hogd
