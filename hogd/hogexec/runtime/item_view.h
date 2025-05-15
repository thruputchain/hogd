#pragma once

#include <cassert>
#include <concepts>
#include <optional>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/status.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "syscall.hh"

namespace hogd {

class SnapshotStorageItem;

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

  constexpr static StorageItemType kItemType =
      StorageItemType::kItemTypeRetrocausalQueue;

  SystemStatus<std::optional<QueueEntry>>
  get_next_item(ExecutionModeData &execution_mode_data, bool advance_cursor);

  void insert_entry(QueueEntry const &entry);

private:
  SystemStatus<const RetrocausalQueueBlock *>
  get_next_block(uint64_t search_start_index) const;

  SystemStatus<std::optional<QueueEntry>>
  get_next_concurrent_entry(uint32_t index,
                            ExecutionModeData &execution_mode_data) const;

  const QueueEntry *get_next_self_entry(uint32_t index) const;

  void add_empty_consume_delta();

  RetrocausalQueueDelta queue_delta_{};
  uint32_t self_consumed_index_ = 0;
  uint32_t locally_consumed_index_ = 0;

  SnapshotStorageItem &base_item_;
};

class AssetBalanceItem {
public:
  AssetBalanceItem(int128_t initial_asset_balance)
    : asset_balance_(initial_asset_balance),
      initial_asset_balance_(initial_asset_balance)
  {
    assert(initial_asset_balance >= 0);
  }

  StorageDelta get_delta() const;

  constexpr static StorageItemType kItemType =
      StorageItemType::kItemTypeAssetBalance;

  SyscallStatus<void> validate() const;

  int128_t asset_balance_;

private:
  int128_t initial_asset_balance_;
};

class NativeTokenBalanceItem {
public:
  NativeTokenBalanceItem(int128_t initial_native_token_balance)
    : native_token_balance_(initial_native_token_balance),
      initial_native_token_balance_(initial_native_token_balance)
  {}

  StorageDelta get_delta() const;

  constexpr static StorageItemType kItemType =
      StorageItemType::kItemTypeAssetBalance;

  SyscallStatus<void> validate() const
  {
    return native_token_balance_ >= 0
               ? SyscallStatus<void>{}
               : SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }

  void add_deterministic_native_token_balance(int128_t amount);
  void add_nondeterministic_native_token_balance(
      Nondeterministic<int128_t> const &amount);

private:
  Nondeterministic<int128_t> additional_subtract_amount_;
  int128_t native_token_balance_;
  int128_t initial_native_token_balance_;
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

  // Returns a pointer to the item view if it is of the given type,
  // otherwise returns nullptr.
  //
  // Some types, like the NNInt, use a validate() method to ensure
  // that no bad data is written.  For those types, to make sure that
  // we do not forget to validate, we return a const pointer
  // and thus force the caller to go through the set() method
  // to modify the item.
  //
  // However, some data types, like the RetrocausalQueueItem, are
  // nontrivially costly to copy.  The ItemView*s that are returned from
  // InvocationFrameStack::get_item_view() are already copy-on-write (within a
  // frame), so it is safe to modify them without worrying about frame reverts.
  // It's still required to go through the set() method to change the type.
  // These types cannot have a validate() method, as that nonconst interface
  // does not allow us to statically enforce that the validate is called.
  template<typename ItemType> auto *get()
  {
    if constexpr (requires(ItemType const &item) { item.validate(); }) {
      return std::holds_alternative<ItemType>(delta_args_)
                 ? const_cast<const ItemType *>(
                       &std::get<ItemType>(delta_args_))
                 : nullptr;
    }
    else {
      return std::holds_alternative<ItemType>(delta_args_)
                 ? &std::get<ItemType>(delta_args_)
                 : nullptr;
    }
  }

  template<typename ItemType> SyscallStatus<void> set(ItemType const &type)
  {
    static_assert(!std::same_as<ItemType, UnmodifiedItem>);
    if constexpr (requires(ItemType const &item) { item.validate(); }) {
      auto validate_result = type.validate();
      if (!validate_result) {
        // Validation check asserts that the written item type is well-formed.
        return validate_result;
      }
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

    delta_args_.emplace<ItemType>(type);
    return {};
  }

  std::optional<StorageDelta> get_delta() const;

private:
  SyscallStatus<void>
  can_accept_type(std::optional<StorageItemType> item_type) const;

  std::variant<UnmodifiedItem, DeletedItem, BytestringItem, NNIntItem,
               RetrocausalQueueItem, AssetBalanceItem>
      delta_args_;
};

} // namespace hogd
