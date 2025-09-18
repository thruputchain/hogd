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
    case StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance:
      if (item_type != StorageItemType::kItemTypeNonnegativeInt) {
        error("item type is not valid for key type: {} with {}", key_type,
              item_type);
        assert(false);
      }
      break;
    case StorageKeyType::kStorageKeyTypeAccountAuthData:
      if (item_type != StorageItemType::kItemTypeAuthData) {
        error("item type is not valid for key type: {} with {}", key_type,
              item_type);
        assert(false);
      }
      break;
    case StorageKeyType::kStorageKeyTypeAddressMetadata:
      if (item_type != StorageItemType::kItemTypeDeployedContractHash) {
        error("item type is not valid for key type: {} with {}", key_type,
              item_type);
        assert(false);
      }
      break;
    case StorageKeyType::kStorageKeyTypeContractData:
      if (item_type == StorageItemType::kItemTypeAuthData ||
          item_type == StorageItemType::kItemTypeDeployedContractHash) {
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
    case StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance:
      if (delta_type != StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd &&
          delta_type != StorageDeltaType::kDeltaTypeNonnegativeIntAdd) {
        error("delta type is not valid for key type: {} with {}", key_type,
              delta_type);
        assert(false);
      }
      break;
    case StorageKeyType::kStorageKeyTypeAccountAuthData:
      if (delta_type != StorageDeltaType::kDeltaTypeAuthDataChange) {
        error("delta type is not valid for key type: {} with {}", key_type,
              delta_type);
        assert(false);
      }
      break;
    case StorageKeyType::kStorageKeyTypeAddressMetadata:
      if (delta_type != StorageDeltaType::kDeltaTypeDeployContract) {
        error("delta type is not valid for key type: {} with {}", key_type,
              delta_type);
        assert(false);
      }
      break;
    case StorageKeyType::kStorageKeyTypeContractData:
      if (delta_type == StorageDeltaType::kDeltaTypeAuthDataChange ||
          delta_type == StorageDeltaType::kDeltaTypeDeployContract) {
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
