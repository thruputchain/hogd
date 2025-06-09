#pragma once

#include <algorithm>
#include <cassert>
#include <concepts>
#include <limits>
#include <optional>
#include <variant>
#include <set>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/item_view_type.h"
#include "hogd/hogexec/runtime/status.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

class SnapshotStorageItem;

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

class NativeTokenBalanceItem {
public:
  NativeTokenBalanceItem(int128_t initial_native_token_balance)
    : native_token_balance_(initial_native_token_balance),
      initial_native_token_balance_(initial_native_token_balance)
  {
    assert(initial_native_token_balance >= 0);
    assert(initial_native_token_balance <=
           int128_t{kTotalHogSupply} * int128_t{kHogDivisibility});
  }

  // This is an overly aggressive assertion.
  // We require that the maximum possible native token balance required to be
  // locked for any given storage item is at most the total maximum supply of
  // native tokens. We then also require that no action by the user can transfer
  // more than the total supply of native tokens at any one time, and the user
  // cannot try to hold more that the total amount of native tokens in the
  // system at any one time. As such, we are guaranteed that for any set of
  // nondeterministic native token required reservations (from try_reserve in
  // the db), the sum of those reservations, plus any deltas added manually,
  // will never overflow int128_t and thus will always fit into a StorageDelta.
  // (assuming one transaction won't modify more than 2^64 storage items).
  static_assert(std::numeric_limits<int128_t>::max() >=
                int128_t{kTotalHogSupply} * int128_t{kHogDivisibility} *
                    int128_t{std::numeric_limits<uint64_t>::max()});

  StorageDelta get_delta() const;

  constexpr static ItemViewType kItemType = ItemViewType::kNativeTokenBalance;

  SyscallStatus<void> validate() const
  {
    return native_token_balance_ >= 0
               ? SyscallStatus<void>{}
               : SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }

  // Returns the balance that the user sees.
  // This is the balance, without applying any of the nondeterministic locks.
  int128_t get_user_visible_balance() const { return native_token_balance_; }

  void add_deterministic_native_token_balance(int128_t amount);
  // Mark the requirement that a certain amount of the native tokens be locked,
  // based on the response of the db to a try_reserve call.
  // The input should be nonnegative (the db returns the additional amount to be
  // locked).
  void add_nondeterministic_native_token_lock(
      Nondeterministic<int128_t> const &amount);

private:
  Nondeterministic<int128_t> additional_subtract_amount_ = 0;
  int128_t native_token_balance_;
  int128_t initial_native_token_balance_;
};

class ReplayCacheItem {
public:
  ReplayCacheItem(ReplayCacheMetadata metadata) : metadata_(metadata)
  {}

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
  constexpr static std::optional<StorageDelta> get_delta()
  {
    return std::nullopt;
  }

  std::optional<ItemViewType> get_item_type() const;

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

    if (auto type_match = can_accept_type<ItemType>(); !type_match) {
      // no type mismatch allowed; if you want to change an object's type,
      // you must explicitly delete it first.
      return type_match;
    }

    delta_args_.emplace<ItemType>(type);
    return {};
  }

  std::optional<StorageDelta> get_delta() const;

  Nondeterministic<int128_t> get_locked_storage_cost() const
  {
    return locked_storage_cost_;
  }

  void set_locked_storage_cost(Nondeterministic<int128_t> cost)
  {
    locked_storage_cost_ = cost;
  }

private:
  template<typename ItemType> SyscallStatus<void> can_accept_type() const
  {
    static_assert(!std::same_as<ItemType, UnmodifiedItem>);

    // writing a deletion is always allowed
    if constexpr (std::same_as<ItemType, DeletedItem>) {
      return {};
    }
    else {
      // DeletedItem has no kItemType, so we need the constexpr else
      // to make this compile.

      // if item is nonexistent, any write is allowed
      if (std::holds_alternative<DeletedItem>(delta_args_)) {
        return {};
      }
      // if item is of the same type, any write is allowed
      if (std::holds_alternative<ItemType>(delta_args_)) {
        return {};
      }

      // if item is unmodified, writes allowed if types match
      if (std::holds_alternative<UnmodifiedItem>(delta_args_)) {
        auto item_type = std::get<UnmodifiedItem>(delta_args_).get_item_type();
        if (!item_type) {
          return {};
        }

        if (*item_type == ItemType::kItemType) {
          return {};
        }

        // debug assertions
        if constexpr (is_debug_build()) {
          debug_assert_view_types(
              std::get<UnmodifiedItem>(delta_args_).base_item_,
              ItemType::kItemType);
        }
      }
    }

    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }

  static void debug_assert_view_types(SnapshotStorageItem const &base_item,
                                      ItemViewType item_type);

  std::variant<UnmodifiedItem, DeletedItem, BytestringItem, NNIntItem,
               RetrocausalQueueItem, NativeTokenBalanceItem, ReplayCacheItem>
      delta_args_;
  Nondeterministic<int128_t> locked_storage_cost_ = 0;
};

} // namespace hogd
