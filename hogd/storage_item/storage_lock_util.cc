#include "hogd/storage_item/storage_lock_util.h"

#include <algorithm>
#include <stdexcept>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "storage_lock.hh"

namespace hogd {

namespace {
StorageItemTypeClass
delta_to_typeclass(StorageDelta const &delta,
                   StorageItemWrapper const *committed_item)
{
  switch (delta.type()) {
  case StorageDeltaType::kDeltaTypeBytestringSet:
  {
    auto out = StorageItemTypeClass(StorageItemType::kItemTypeBytestring);
    out.bytestring() = delta.bytestring();
    return out;
  }
  case StorageDeltaType::kDeltaTypeNonnegativeIntAdd:
  {
    int128_t set_value = [committed_item]() -> int128_t {
      if (!committed_item) {
        return 0;
      }
      if (committed_item->type() != StorageItemType::kItemTypeNonnegativeInt) {
        return 0;
      }
      return committed_item->nnint();
    }();

    auto out = StorageItemTypeClass(StorageItemType::kItemTypeNonnegativeInt);
    out.nnint_set() = to_xdr(set_value);
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
  case StorageDeltaType::kDeltaTypeReplayCacheModify:
    return StorageItemTypeClass(StorageItemType::kItemTypeReplayCache);
  case StorageDeltaType::kDeltaTypeAuthDataChange:
    return StorageItemTypeClass(StorageItemType::kItemTypeAuthData);
  default:
    throw std::runtime_error("unimplemented");
  }
  // unreachable
}
} // namespace

StorageLock
delta_to_lock(StorageDelta const &delta, BlockExecutionMode mode,
              StorageItemWrapper const *committed_item)
{
  StorageLock lock(StorageLockType::kSetItemTypeClass);
  lock.full_typeclass_lock().typeclass =
      delta_to_typeclass(delta, committed_item);
  switch (delta.type()) {
  case StorageDeltaType::kDeltaTypeBytestringSet:
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kItemTypeBytestring);
    return lock;
  case StorageDeltaType::kDeltaTypeNonnegativeIntAdd:
  case StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd:
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kItemTypeNonnegativeInt);
    if (mode == BlockExecutionMode::kBlockExecutionModeValidation) {
      lock.full_typeclass_lock().extra_lock_data.nnint_bounds_lock() =
          NNIntBoundsLock{.add_amount = to_xdr(int128_t{0}),
                          .subtract_amount = to_xdr(int128_t{0})};
    }
    else {
      int128_t add_amount = [&delta]() {
        if (delta.type() == StorageDeltaType::kDeltaTypeNonnegativeIntAdd) {
          return from_xdr(delta.nnint_add());
        }
        return from_xdr(delta.nnint().add_value);
      }();
      lock.full_typeclass_lock().extra_lock_data.nnint_bounds_lock() =
          NNIntBoundsLock{.add_amount =
                              to_xdr(std::max<int128_t>(0, add_amount)),
                          .subtract_amount =
                              to_xdr(std::min<int128_t>(0, add_amount))};
    }
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
  case StorageDeltaType::kDeltaTypeReplayCacheModify:
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kItemTypeReplayCache);
    lock.full_typeclass_lock().extra_lock_data.nonces() =
        delta.replay_cache_modify().nonces;
    return lock;
  case StorageDeltaType::kDeltaTypeAuthDataChange:
  {
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kItemTypeAuthData);
    AuthDataLock auth_data_lock;
    for (auto const& signer : delta.auth_data_change().auth_data_change_signers) {
      auth_data_lock.modified_key_ids.push_back(signer.public_key_id);
    }
    lock.full_typeclass_lock().extra_lock_data.auth_data_lock() = AuthDataLock{
        .modified_key_ids = auth_data_lock.modified_key_ids};
    return lock;
  }
  default:
    throw std::runtime_error("unimplemented");
  }
}

} // namespace hogd
