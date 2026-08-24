#pragma once

#include <cassert>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

constexpr void
check_item_key_type(StorageKeyType key_type, StorageItemType item_type)
{
  if constexpr (is_debug_build()) {
    switch (key_type) {
    case StorageKeyType::FreeNativeTokenBalance:
      if (item_type != StorageItemType::kItemTypeNonnegativeInt) {
        error("item type is not valid for key type: {} with {}", key_type,
              item_type);
        assert(false);
      }
      break;
    case StorageKeyType::AuthData:
      if (item_type != StorageItemType::kItemTypeAuthData) {
        error("item type is not valid for key type: {} with {}", key_type,
              item_type);
        assert(false);
      }
      break;
    case StorageKeyType::Metadata:
      if (item_type != StorageItemType::kItemTypeDeployedContractHash) {
        error("item type is not valid for key type: {} with {}", key_type,
              item_type);
        assert(false);
      }
      break;
    case StorageKeyType::ContractData:
      if (item_type == StorageItemType::kItemTypeAuthData ||
          item_type == StorageItemType::kItemTypeDeployedContractHash) {
        error("item type is not valid for key type: {} with {}", key_type,
              item_type);
        assert(false);
      }
      break;
    case StorageKeyType::ReplayCache:
      if (item_type != StorageItemType::kItemTypeReplayCacheData) {
        error("item type is not valid for key type: {} with {}", key_type,
              item_type);
        assert(false);
      }
      break;
    default:
      return;
      // item type is valid for key type
    }
  }
}

constexpr void
check_delta_type(StorageKeyType key_type, StorageDeltaType delta_type)
{
  if constexpr (is_debug_build()) {
    switch (key_type) {
    case StorageKeyType::FreeNativeTokenBalance:
      if (delta_type != StorageDeltaType::kDeltaTypeNonnegativeIntAdd) {
        error("delta type is not valid for key type: {} with {}", key_type,
              delta_type);
        assert(false);
      }
      break;
    case StorageKeyType::AuthData:
      if (delta_type != StorageDeltaType::kDeltaTypeAuthDataChange) {
        error("delta type is not valid for key type: {} with {}", key_type,
              delta_type);
        assert(false);
      }
      break;
    case StorageKeyType::Metadata:
      if (delta_type != StorageDeltaType::kDeltaTypeDeployContract) {
        error("delta type is not valid for key type: {} with {}", key_type,
              delta_type);
        assert(false);
      }
      break;
    case StorageKeyType::ContractData:
      if (delta_type == StorageDeltaType::kDeltaTypeAuthDataChange ||
          delta_type == StorageDeltaType::kDeltaTypeDeployContract) {
        error("delta type is not valid for key type: {} with {}", key_type,
              delta_type);
        assert(false);
      }
      break;
    case StorageKeyType::ReplayCache:
      // A ReplayCache key only ever carries a nonce consumption. Any other
      // delta would later be staged through the contract-data path (the item
      // lock is chosen by the change variant, not the key type) and trip the
      // key-type assert in make_contract_data_key at block close. Reject it
      // here so a buggy executor surfaces at the RPC boundary instead.
      if (delta_type != StorageDeltaType::kDeltaTypeConsumeUniquifier) {
        error("delta type is not valid for key type: {} with {}", key_type,
              delta_type);
        assert(false);
      }
      break;
    default:
      return;
      // delta type is valid for key type
    }
  }
}

} // namespace hogd
