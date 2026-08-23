#pragma once

#include <cassert>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <variant>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/execution_mode.h"
#include "hogd/hog_exec/runtime/item/item_view_type.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"

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

  std::optional<int128_xdr> base_value_;
  int128_xdr add_value_;
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
  RetrocausalQueueItem(const SnapshotStorageItem &base_item)
    : base_item_(&base_item)
  {}

  RetrocausalQueueItem(RetrocausalQueueItem &&other) noexcept = default;
  RetrocausalQueueItem &
  operator=(RetrocausalQueueItem &&other) noexcept = default;
  RetrocausalQueueItem(RetrocausalQueueItem const &other) = default;
  RetrocausalQueueItem &operator=(RetrocausalQueueItem const &other) = default;

  static RetrocausalQueueItem
  make_fully_consumed_item(const SnapshotStorageItem &base_item,
                           x23::Vec<QueueEntry> consumed_entries);

  StorageDelta
  get_delta(std::span<QueueEntry const> observed_entries = {}) const;

  constexpr static ItemViewType kItemType = ItemViewType::kRetrocausalQueue;

  // Returns a pointer to the next entry to consume, or nullptr when there is
  // nothing to consume (empty read or per-tx cap reached); a ValidationFailure
  // on malformed replay data. The pointer aliases into owned storage (the
  // snapshot block, this item's self-insert delta, or the proposal/validation
  // observation buffer) and is valid until the next RCQ mutation -- callers
  // copy it into the syscall result before returning to the contract.
  x23::Async<SystemStatus<const QueueEntry *>>
  get_next_item(ExecutionModeData &execution_mode_data, bool advance_cursor);

  SyscallStatus<void> insert_entry(QueueEntry const &entry);

  uint32_t get_retrocausally_consumed_count() const
  {
    return locally_consumed_index_;
  }

private:
  x23::Async<SystemStatus<const RetrocausalQueueBlock *>>
  get_next_block(uint64_t search_start_index) const;

  x23::Async<SystemStatus<const QueueEntry *>>
  get_next_concurrent_entry(uint32_t index,
                            ExecutionModeData &execution_mode_data) const;

  // Proposal-mode miss path for the above: the only arm that reads the
  // database.  [proposal_data] aliases the caller's ExecutionModeData and must
  // outlive the call.
  x23::Async<SystemStatus<const QueueEntry *>>
  get_next_concurrent_entry_proposing(uint32_t index,
                                      ProposalModeData &proposal_data) const;

  const QueueEntry *get_next_self_entry(uint32_t index) const;

  void construct_consume_delta();

  RetrocausalQueueDelta queue_delta_{};

  // Can't overflow locally_consumed_index_ in a uint16
  static_assert(std::numeric_limits<uint16_t>::max() >=
                kRetrocausalQueueBlockEntriesMaxLen);

  // count of entries consumed that were inserted by this transaction
  uint16_t self_consumed_index_ = 0;
  // count of retrocausally consumed entries from other transactions
  uint16_t locally_consumed_index_ = 0;

  // For (in future PR) tracking storage costs
  uint32_t past_block_consumed_count_ = 0;

  const SnapshotStorageItem *base_item_;
};

class ReplayCacheItem {
public:
  ReplayCacheItem(Uniquifier consumed_uniquifier)
    : consumed_uniquifier_(consumed_uniquifier)
  {}

  ReplayCacheItem(ReplayCacheItem &&other) noexcept = default;
  ReplayCacheItem &operator=(ReplayCacheItem &&other) noexcept = default;
  ReplayCacheItem(ReplayCacheItem const &other) = default;
  ReplayCacheItem &operator=(ReplayCacheItem const &other) = default;

  StorageDelta get_delta() const;
  constexpr static SyscallStatus<void> validate() { return {}; }

  constexpr static ItemViewType kItemType = ItemViewType::kReplayCache;

private:
  Uniquifier consumed_uniquifier_;
};

class AddressAuthDataItem {
public:
  AddressAuthDataItem()
    : internal_auth_data_(std::make_unique<InternalAuthData>())
  {}

  AddressAuthDataItem(AddressAuthDataItem const &other)
    : internal_auth_data_(
          std::make_unique<InternalAuthData>(*other.internal_auth_data_))
  {}
  AddressAuthDataItem(AddressAuthDataItem &&other) noexcept = default;

  AddressAuthDataItem &
  operator=(AddressAuthDataItem &&other) noexcept = default;
  AddressAuthDataItem &operator=(AddressAuthDataItem const &other);

  StorageDelta get_delta() const;

  constexpr static ItemViewType kItemType = ItemViewType::kAddressAuthData;

  void change_public_key(uint32_t key_index, PublicKeyConfigOpt new_key)
  {
    assert(internal_auth_data_);
    internal_auth_data_->pk_changes_[key_index] = std::move(new_key);
  }

  void set_required_weight(Weight w)
  {
    assert(internal_auth_data_);
    internal_auth_data_->required_weight_ = w;
  }

private:
  struct InternalAuthData {
    std::unordered_map<uint32_t, PublicKeyConfigOpt> pk_changes_;
    std::optional<Weight> required_weight_;
  };
  std::unique_ptr<InternalAuthData> internal_auth_data_;
};

// Represent state of an ItemView that has not been modified during
// the current transaction.
// Should never be created outside of ItemView, but might be
// read outside.
class UnmodifiedItem {
public:
  UnmodifiedItem() : base_item_(std::nullopt) {}

  UnmodifiedItem(UnmodifiedItem &&other) noexcept = default;
  UnmodifiedItem &operator=(UnmodifiedItem &&other) noexcept = default;
  UnmodifiedItem(UnmodifiedItem const &other) = default;
  UnmodifiedItem &operator=(UnmodifiedItem const &other) = default;

  static std::optional<StorageDelta>
  get_delta(std::span<QueueEntry const> observed_entries = {})
  {
    if (!observed_entries.empty()) {
      StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
      delta.retrocausal_queue().observed_concurrent_entries = {
          observed_entries.begin(), observed_entries.end()};
      return delta;
    }
    return std::nullopt;
  }

  std::optional<ItemViewType> get_item_type() const;

  // Populates base_item_ from the snapshot cache.
  x23::Async<SystemStatus<void>> load(StorageSnapshotCache &snapshot_cache,
                                      AddressAndKey const &addrkey) const;

private:
  // Shared tail of both load() paths.
  SystemStatus<void>
  store_loaded(SystemStatus<const SnapshotStorageItem *> const &r) const;

public:
  bool is_loaded() const { return base_item_.has_value(); }

  const SnapshotStorageItem &base_item() const
  {
    CHECK(base_item_.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    CHECK_PTR(*base_item_);
    return **base_item_;
    // NOLINTEND(bugprone-unchecked-optional-access)
  }

private:
  mutable std::optional<const SnapshotStorageItem *> base_item_;
};

// Maintain the modifications of one InvocationFrame to one address.
// Provides for a transaction a locally consistent view of a storage item,
// with modifications to the item up to that point in the transaction applied to
// it.
class ItemView {
public:
  ItemView() : delta_args_(UnmodifiedItem()) {}

  ItemView(ItemView &&other) noexcept = default;
  ItemView &operator=(ItemView &&other) noexcept = default;
  ItemView(ItemView const &other) = default;
  ItemView &operator=(ItemView const &other) = default;

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

  template<typename ItemType>
  SyscallStatus<void>
  set(ItemType const &type,
      StorageDeltaFlagSet flags = std::to_underlying(StorageDeltaFlags::None))
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
    flags_ = flags;
    return {};
  }

  std::pair<std::optional<StorageDelta>, StorageDeltaFlagSet>
  get_delta(std::span<QueueEntry const> observed_entries = {}) const;

  void sync_retrocausally_consumed(RetrocausalQueueItem const &item)
  {
    retrocausally_consumed_count_ = item.get_retrocausally_consumed_count();
  }

  uint32_t get_retrocausally_consumed_count() const
  {
    return retrocausally_consumed_count_;
  }

private:
  std::variant<UnmodifiedItem, DeletedItem, BytestringItem, NNIntItem,
               NativeTokenBalanceItem, RetrocausalQueueItem, ReplayCacheItem,
               AddressAuthDataItem>
      delta_args_;
  StorageDeltaFlagSet flags_ = std::to_underlying(StorageDeltaFlags::None);

  // There is a niche edge case, in which a contract deletes a queue and then
  // recreates it. In this case we need to know how which entries were
  // retrocausally consumed before the queue was deleted. Otherwise we could
  // double consume an entry, i.e. before and after the queue was deleted. We
  // need the actual entries, not just the count, in order to construct the
  // final delta, but we can fetch those from the snapshot storage item.
  uint32_t retrocausally_consumed_count_ = 0;
};

} // namespace hogd
