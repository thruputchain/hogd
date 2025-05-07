#pragma once

#include <cassert>
#include <optional>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/snapshot_storage_item.h"
#include "hogd/hogexec/runtime/status.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"

namespace hogd {

// Each of the item types has the following methods:
// get_object_type()
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
  StorageDelta get_delta() const;
  SyscallStatus<void> validate() const;

  constexpr static StorageItemType kItemType =
      StorageItemType::kItemTypeBytestring;

  Bytestring bytestring_;
};

// State of an item that is a nonnegative int
class NNIntItem {
public:
  StorageDelta get_delta() const;
  SyscallStatus<void> validate() const;
  constexpr static StorageItemType kItemType =
      StorageItemType::kItemTypeNonnegativeInt;

  int128_t base_value_;
  int128_t add_value_;
};

// Represent state of item that has been deleted.
class DeletedItem {
public:
  constexpr static StorageDelta get_delta()
  {
    return StorageDelta(StorageDeltaType::kDeltaTypeDelete);
  }

  constexpr static SyscallStatus<void> validate() { return {}; }

  constexpr static std::optional<StorageItemType> kItemType = std::nullopt;
};

class RetrocausalQueueItem {
public:
  RetrocausalQueueItem(SnapshotStorageItem &base_item) : base_item_(base_item)
  {}

  StorageDelta get_delta() const
  {
    StorageDelta delta =
        StorageDelta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue() = queue_delta_;
    return delta;
  }

  constexpr static SyscallStatus<void> validate() { return {}; }

  constexpr static StorageItemType kItemType =
      StorageItemType::kItemTypeRetrocausalQueue;

  HostFnStatus<std::optional<QueueEntry>>
  get_next_item(ExecutionModeData &execution_mode_data, bool advance_cursor);

  void insert_entry(QueueEntry const &entry);

private:
  SystemStatus<const RetrocausalQueueBlock *>
  get_next_block(uint64_t search_start_index) const;

  HostFnStatus<std::optional<QueueEntry>>
  get_next_concurrent_entry(uint32_t index,
                            ExecutionModeData &execution_mode_data) const;

  const QueueEntry *get_next_self_entry(uint32_t index) const;

  RetrocausalQueueDelta queue_delta_{};
  uint32_t self_consumed_index_ = 0;
  uint32_t locally_consumed_index_ = 0;

  SnapshotStorageItem &base_item_;
};

// Represent state of an ItemView that has not been modified during
// the current transaction.
// Should never be created outside of ItemView, but might be
// read outside.
class UnmodifiedItem {
public:
  constexpr static std::optional<StorageDelta> get_delta()
  {
    return std::nullopt;
  }

  // no validate() method, to ensure that set<UnmodifiedType> does not compile

  std::optional<StorageItemType> get_item_type() const;

  SnapshotStorageItem &base_item_;
};

// Maintain the modifications of one InvocationFrame to one address.
// Provides for a transaction a locally consistent view of a storage item,
// with modifications to the item up to that point in the transaction applied to
// it.
class ItemView {
public:
  ItemView(SnapshotStorageItem *base_item)
    : delta_args_(UnmodifiedItem{.base_item_ = *base_item})
  {}

  std::optional<StorageItemType> get_item_type() const;

  template<typename ItemType> const auto *get()
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

    // The only item type that needs the get_item_type() method,
    // instead of the constant, is UnmodifiedItem, which is never passed through
    // this method anyways.
    auto validate_type = can_accept_type(type.kItemType);
    if (!validate_type) {
      // no type mismatch; if you want to change an object's type,
      // you must explicitly delete it first.
      return validate_type;
    }

    delta_args_ = type;
    return {};
  }

  std::optional<StorageDelta> get_delta() const;

private:
  SyscallStatus<void>
  can_accept_type(std::optional<StorageItemType> item_type) const;

  std::variant<UnmodifiedItem, DeletedItem, BytestringItem, NNIntItem>
      delta_args_;
};

} // namespace hogd
