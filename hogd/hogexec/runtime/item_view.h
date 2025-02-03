#pragma once

#include <cassert>
#include <optional>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "storage_item.hh"

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
  RetrocausalQueueItem(
      StorageSnapshotCacheEntry const *queue_snapshot_cache_entry,
      NondeterministicReadCacheEntry &nondeterministic_read_cache_entry)
    : queue_snapshot_cache_entry_(queue_snapshot_cache_entry),
      nondeterministic_read_cache_entry_(nondeterministic_read_cache_entry)
  {
    if (queue_snapshot_cache_entry_) {
      assert(queue_snapshot_cache_entry_->type() ==
             StorageItemType::kRetrocausalQueue);
    }
  }

  FlaggedStorageDelta get_delta() const
  {
    StorageDelta delta =
        StorageDelta(StorageDeltaType::kRetrocausalQueueModify);
    delta.retrocausal_queue() = queue_delta_;
    return FlaggedStorageDelta{.delta = delta, .flags = 0};
  }

  constexpr static SyscallStatus<void> validate() { return {}; }

  constexpr static StorageItemType get_item_type()
  {
    return StorageItemType::kRetrocausalQueue;
  }

  HostFnStatus<std::optional<QueueEntry>>
  get_next_item(ExecutionModeData &execution_mode_data) const;
  HostFnStatus<void> advance_cursor(ExecutionModeData &execution_mode_data);
  void insert_entry(QueueEntry const &entry);

private:
  const RetrocausalQueueBlock *
  get_next_block(uint64_t search_start_index) const;

  HostFnStatus<std::optional<QueueEntry>>
  get_next_concurrent_entry(uint32_t index,
                            ExecutionModeData &execution_mode_data) const;

  HostFnStatus<const QueueEntry *>
  get_next_self_entry(uint32_t index,
                      ExecutionModeData &execution_mode_data) const;

  RetrocausalQueueDelta queue_delta_;
  uint32_t self_consumed_index_ = 0;
  uint32_t locally_consumed_index_ = 0;
  StorageSnapshotCacheEntry const *queue_snapshot_cache_entry_;
  NondeterministicReadCacheEntry &nondeterministic_read_cache_entry_;
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

  template<typename ItemType> SyscallStatus<void> set(ItemType type)
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

    delta_args_.emplace<ItemType>(std::move(type));
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
