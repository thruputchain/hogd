#pragma once

#include <optional>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"

namespace hogd {

// Each of the item types has the following methods:
// get_item_type()
//   Return the type of the storage object, as it exists in the current view
// get_delta()
//   Return the storage delta that would be produced if this item view commits
//   to the database.
// validate()
//   Validate the delta.  Called within set().  If validate() returns an error,
//   the set() call in the ItemView is a no-op.

// Representing the internal state of a bytestring item.
class BytestringItem {
public:
  FlaggedStorageDelta get_delta() const;
  SyscallStatus<void> validate() const;

  constexpr static StorageItemType get_item_type()
  {
    return StorageItemType::kBytestring;
  }

  Bytestring bytestring_;
  bool lock_exclusive_ = false;
};

// State of an item that is a nonnegative int
class NNIntItem {
public:
  FlaggedStorageDelta get_delta() const;
  SyscallStatus<void> validate() const;

  constexpr static StorageItemType get_item_type()
  {
    return StorageItemType::kNonnegativeInt;
  }

  int128_t base_value_;
  int128_t add_value_;
};

// Represent state of item that has been deleted.
class DeletedItem {

public:
  constexpr static FlaggedStorageDelta get_delta()
  {
    return FlaggedStorageDelta{.delta = StorageDelta(StorageDeltaType::kDelete),
                               .flags = 0};
  }

  constexpr static SyscallStatus<void> validate() { return {}; }

  constexpr static std::optional<StorageItemType> get_item_type()
  {
    return std::nullopt;
  }
};

class RetrocausalQueueItem {
public:
  RetrocausalQueueDelta queue_delta;

  FlaggedStorageDelta get_delta() const
  {
    StorageDelta delta =
        StorageDelta(StorageDeltaType::kRetrocausalQueueModify);
    delta.retrocausal_queue() = queue_delta;
    return FlaggedStorageDelta{.delta = delta, .flags = 0};
  }

  constexpr static SyscallStatus<void> validate() { return {}; }

  constexpr static StorageItemType get_item_type()
  {
    return StorageItemType::kRetrocausalQueue;
  }
};

// Represent state of an ItemView that has not been modified during
// the current transaction.
// Should never be created outside of ItemView, but might be
// read outside.
class UnmodifiedItem {
public:
  constexpr static std::optional<FlaggedStorageDelta> get_delta()
  {
    return std::nullopt;
  }

  // no validate() method, to ensure that set<UnmodifiedType> does not compile

  std::optional<StorageItemType> get_item_type() const;

  const StorageSnapshotCacheEntry *const base_item_;
};

// Maintain the modifications of one InvocationFrame to one address.
// Provides for a transaction a locally consistent view of a storage item,
// with modifications to the item up to that point in the transaction applied to
// it.
class ItemView {
public:
  ItemView(const StorageSnapshotCacheEntry *base_item)
    : delta_args_(UnmodifiedItem{.base_item_ = base_item})
  {}

  std::optional<StorageItemType> get_item_type() const;

  template<typename ItemType> const auto *get() const
  {
    return std::holds_alternative<ItemType>(delta_args_)
               ? &std::get<ItemType>(delta_args_)
               : nullptr;
  }

  template<typename ItemType> SyscallStatus<void> set(ItemType const &type)
  {
    auto validate_result = type.validate();
    if (!validate_result) {
      // Validation check asserts that the written item type is well-formed.
      return validate_result;
    }

    auto validate_type = can_accept_type(type.get_item_type());
    if (!validate_type) {
      // no type mismatch; if you want to change an object's type,
      // you must explicitly delete it first.
      return validate_type;
    }

    delta_args_ = type;
    return {};
  }

  std::optional<FlaggedStorageDelta> get_delta() const;

  SyscallStatus<void>
  can_accept_type(std::optional<StorageItemType> item_type) const;

private:
  std::variant<UnmodifiedItem, DeletedItem, BytestringItem, NNIntItem,
               RetrocausalQueueItem>
      delta_args_;
};

} // namespace hogd
