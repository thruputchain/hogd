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
  case StorageDeltaType::kBytestringSet:
  {
    auto out = StorageItemTypeClass(StorageItemType::kBytestring);
    out.bytestring() = delta.bytestring();
    return out;
  }
  case StorageDeltaType::kNonnegativeIntSetAdd:
  {
    auto out = StorageItemTypeClass(StorageItemType::kNonnegativeInt);
    out.nnint_set() = delta.nnint().set_value;
    return out;
  }
  case StorageDeltaType::kDelete:
    return StorageItemTypeClass(StorageItemType::kDeleted);
  case StorageDeltaType::kRetrocausalQueue:
    return StorageItemTypeClass(StorageItemType::kRetrocausalQueue);
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
  case StorageDeltaType::kBytestringSet:
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kBytestring);
    return lock;
  case StorageDeltaType::kNonnegativeIntSetAdd:
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kNonnegativeInt);
    lock.full_typeclass_lock().extra_lock_data.nnint_subtracted_amount() =
        to_xdr(std::min<int128_t>(0, from_xdr(delta.nnint().add_value)));
    return lock;
  case StorageDeltaType::kRetrocausalQueue:
  {
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kRetrocausalQueue);
    if (delta.retrocausal_queue().consume) {
      lock.full_typeclass_lock().extra_lock_data.consume_lock() = 1;
    }
    return lock;
  }
  case StorageDeltaType::kDelete:
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kDeleted);
    return lock;
  default:
    throw std::runtime_error("unimplemented");
  }
}

} // namespace hogd
