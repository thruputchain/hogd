#pragma once

#include <cassert>
#include <optional>
#include <set>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/item_view_type.h"
#include "hogd/hogexec/runtime/status.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

class SnapshotStorageItem;
class StorageSnapshotCache;

// Each of the item types has the following methods:
// get_delta()
//   Return the storage delta that would be produced if this item view commits
//   to the database.
// validate()
//   Validate the delta.  Called within set().  If validate() returns an error,
//   the set() call in the ItemView is a no-op.
// kItemType (static constant)
//   The type of the item view.

// Representing the internal state of a bytestring item.
class BytestringItem {
public:
  StorageDelta get_delta() const;
  SyscallStatus<void> validate() const;

  constexpr static ItemViewType kItemType = ItemViewType::kBytestring;

  Bytestring bytestring_;
};

// State of an item that is a nonnegative int
class NNIntItem {
public:
  StorageDelta get_delta() const;
  SyscallStatus<void> validate() const;
  constexpr static ItemViewType kItemType = ItemViewType::kNNInt;

  std::optional<int128_t> base_value_;
  int128_t add_value_;
};

// State of an item that is a token balance
class NativeTokenBalanceItem : public NNIntItem {
public:
  constexpr static ItemViewType kItemType = ItemViewType::kNativeTokenBalance;
};

// Represent state of item that has been deleted.
class DeletedItem {
public:
  constexpr static StorageDelta get_delta()
  {
    return StorageDelta(StorageDeltaType::kDeltaTypeDelete);
  }

  constexpr static SyscallStatus<void> validate() { return {}; }
};

// Represent state of an item that is a retrocausal queue.
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

  constexpr static ItemViewType kItemType = ItemViewType::kRetrocausalQueue;

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

class ReplayCacheItem {
public:
  ReplayCacheItem(ReplayCacheMetadata metadata) : metadata_(metadata) {}

  StorageDelta get_delta() const;
  constexpr static SyscallStatus<void> validate() { return {}; }

  SyscallStatus<void> try_insert(ReplayNonce nonce)
  {
    if (nonce <= metadata_.minimum_threshold) {
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
    }
    if (inserted_nonces_.contains(nonce)) {
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
    }
    [[maybe_unused]] auto [_, inserted] = inserted_nonces_.insert(nonce);
    assert(inserted);

    return {};
  }

  SyscallStatus<void> raise_minimum_threshold(ReplayNonce new_threshold)
  {
    if (new_threshold < metadata_.minimum_threshold) {
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
    }
    metadata_.minimum_threshold = new_threshold;
    return {};
  }

  constexpr static ItemViewType kItemType = ItemViewType::kReplayCache;

private:
  ReplayCacheMetadata metadata_;
  std::set<ReplayNonce> inserted_nonces_;
};

// Represent state of an ItemView that has not been modified during
// the current transaction.
// Should never be created outside of ItemView, but might be
// read outside.
class UnmodifiedItem {
public:
  UnmodifiedItem(SnapshotStorageItem *base_item) : base_item_(base_item) {}
  UnmodifiedItem() : base_item_(std::nullopt) {}

  constexpr static std::optional<StorageDelta> get_delta()
  {
    return std::nullopt;
  }

  std::optional<ItemViewType> get_item_type() const;

  SystemStatus<void> load(StorageSnapshotCache &snapshot_cache,
                          AddressAndKey const &addrkey) const;
  bool is_loaded() const { return base_item_.has_value(); }

  SnapshotStorageItem &base_item() const
  {
    assert(base_item_.has_value());
    assert(base_item_.value());
    return *base_item_.value(); // NOLINT(bugprone-unchecked-optional-access)
  }

private:
  mutable std::optional<SnapshotStorageItem *> base_item_;
};

// Maintain the modifications of one InvocationFrame to one address.
// Provides for a transaction a locally consistent view of a storage item,
// with modifications to the item up to that point in the transaction applied to
// it.
class ItemView {
public:
  ItemView(SnapshotStorageItem *base_item)
    : delta_args_(UnmodifiedItem(base_item))
  {}
  ItemView() : delta_args_(UnmodifiedItem()) {}

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
    if constexpr (requires(ItemType const &item) { item.validate(); }) {
      if (auto validate_result = type.validate(); !validate_result) {
        // Validation check asserts that the written item type is well-formed.
        return validate_result;
      }
    }

    // We no longer check the type of the input item against the current item.
    // That would require reading from the database, and we cannot do that
    // if we want to allow lazy loading of items.  Implicitly, this means
    // that we allow a write to overwrite the previous type.
    // If the caller cares about checking the type, they should do the check
    // explicitly (this already happens in several of the syscall handlers).
    //
    // We do not perform the check against types other than UnmodifiedItem,
    // as that would create a discrepancy in behavior between the case where
    // the item was written earlier in the transaction and when it was read from
    // snapshot. (following the principle of "sequential contracts should look
    // as normal as possible" when running in a try_invoke/queue loop)

    delta_args_.emplace<ItemType>(type);
    return {};
  }

  std::optional<StorageDelta> get_delta() const;

private:
  std::variant<UnmodifiedItem, DeletedItem, BytestringItem, NNIntItem,
               NativeTokenBalanceItem, RetrocausalQueueItem, ReplayCacheItem>
      delta_args_;
};

} // namespace hogd
