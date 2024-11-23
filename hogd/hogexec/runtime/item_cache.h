#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <variant>

#include "hogd/hogexec/runtime/status.h"
#include "storage_object.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

class ItemCache {
public:
  ItemCache(std::optional<StrippedStorageObject> obj)
    : base_object_(std::move(obj))
  {}

  ItemCache(ItemCache const &parent)
    : base_object_(parent.base_object_), delta_args_(parent.delta_args_)
      // parent's current lock status is our previous lock status
      ,
      previous_lock_(parent.current_lock_), current_lock_(parent.current_lock_)
  {}

  ItemCache(ItemCache &&) = default;

  void delete_object();

  bool exists() const;
  std::optional<StorageObjectType> get_object_type() const;

  SyscallStatus<Bytestring> get_bytestring() const;
  SyscallStatus<uint64_t> get_bytestring_len() const;
  SyscallStatus<void>
  set_bytestring(std::span<const std::byte> input_bytestring);

  SyscallStatus<int64_t> get_nnint64_base_value() const;
  SyscallStatus<int64_t> get_nnint64_value() const;
  SyscallStatus<void> nnint64_set_add(int64_t set_value, int64_t add_value);

  SyscallStatus<void> replay_cache_insert(uint64_t nonce);
  SyscallStatus<void> replay_cache_clear(uint64_t threshold);

  SyscallStatus<MessageQueueEvent> consume_next_mq_event();
  SyscallStatus<void> append_event_to_mq(MessageQueueEvent const &event);

  x23::Vec<StorageDelta> get_deltas() const;

  // locking workflow:
  //
  // When a try_invoke callframe finishes:
  // each modified ItemCache has compute_storage_lock_acquire() called.
  // The runtime attempts to acquire these locks.
  // If successful, the runtime calls mark_locks_acquired() on each ItemCache,
  // and then moves these ItemCache elements to the parent call frame.
  // otherwise, this ItemCache is dropped.
  // mark_locks_acquired() updates the current_lock_ variable internally.
  // An ItemCache object owns the difference between previous_lock_ and
  // current_lock_.
  //
  // When an ItemCache is dropped, due to e.g. its call frame reverting, in
  // _any_ frame, we call compute_storage_lock_release() on each ItemCache, and
  // execute those StorageLockChange objects. Those actions are designed to be
  // infallible (it is a serious design error if they are not).
  std::optional<StorageLockChange> compute_storage_lock_acquire() const;
  std::optional<StorageLockChange> compute_storage_lock_release() const;
  void mark_locks_acquired();

  std::optional<StorageLock> get_stripped_lock() const;

private:
  SyscallStatus<void> type_guard(StorageObjectType expected_type) const;

  std::optional<FullStorageLock> compute_required_lock() const;

  struct BytestringCacheType {
    Bytestring set_bytestring_arg_;

    x23::Vec<StorageDelta> get_deltas() const;

    constexpr static std::optional<StorageObjectType> kObjectType =
        StorageObjectType::kBytestring;
  };

  struct NNInt64CacheType {
    int64_t nnint64_set_arg_ = 0;
    int64_t nnint64_add_sum_ = 0;

    x23::Vec<StorageDelta> get_deltas() const;

    constexpr static std::optional<StorageObjectType> kObjectType =
        StorageObjectType::kNonnegativeInt64;
  };

  struct ReplayCacheCacheType {
    std::optional<uint64_t> new_max_nonce_clear_threshold_;
    std::set<uint64_t> new_nonces_;

    x23::Vec<StorageDelta> get_deltas() const;

    constexpr static std::optional<StorageObjectType> kObjectType =
        StorageObjectType::kReplayCache;
  };

  struct MessageQueueCacheType {
    uint32_t num_messages_consumed_;
    std::set<MessageQueueEvent> new_messages_;

    x23::Vec<StorageDelta> get_deltas() const;

    constexpr static std::optional<StorageObjectType> kObjectType =
        StorageObjectType::kMessageQueue;
  };

  struct DeleteType {
    constexpr static x23::Vec<StorageDelta> get_deltas()
    {
      StorageDelta out;
      out.type(StorageDeltaType::kDelete);
      return {out};
    }

    constexpr static std::optional<StorageObjectType> kObjectType =
        StorageObjectType::kDeleted;
  };

  struct UnsetType {
    static x23::Vec<StorageDelta> get_deltas() { return {}; }

    constexpr static std::optional<StorageObjectType> kObjectType =
        std::nullopt;
  };

  auto *get_bytestring_args(this auto &self)
  {
    if (std::holds_alternative<BytestringCacheType>(self.delta_args_)) {
      return &std::get<BytestringCacheType>(self.delta_args_);
    }
    return static_cast<decltype(&std::get<BytestringCacheType>(
        self.delta_args_))>(nullptr);
  }

  auto *get_nnint64_args(this auto &self)
  {
    if (std::holds_alternative<NNInt64CacheType>(self.delta_args_)) {
      return &std::get<NNInt64CacheType>(self.delta_args_);
    }
    return static_cast<decltype(&std::get<NNInt64CacheType>(self.delta_args_))>(
        nullptr);
  }

  auto *get_replay_cache_args(this auto &self)
  {
    if (std::holds_alternative<ReplayCacheCacheType>(self.delta_args_)) {
      return &std::get<ReplayCacheCacheType>(self.delta_args_);
    }
    return static_cast<decltype(&std::get<ReplayCacheCacheType>(
        self.delta_args_))>(nullptr);
  }

  auto *get_message_queue_args(this auto &self)
  {
    if (std::holds_alternative<MessageQueueCacheType>(self.delta_args_)) {
      return &std::get<MessageQueueCacheType>(self.delta_args_);
    }
    return static_cast<decltype(&std::get<MessageQueueCacheType>(
        self.delta_args_))>(nullptr);
  }

  std::optional<StrippedStorageObject> base_object_;
  std::variant<UnsetType, DeleteType, BytestringCacheType, NNInt64CacheType,
               ReplayCacheCacheType, MessageQueueCacheType>
      delta_args_ = UnsetType{};

  // status of any storage lock acquired in a parent call frame
  const std::optional<FullStorageLock> previous_lock_ = std::nullopt;

  // lock that is OWNED, possibly not including locally applied deltas.
  std::optional<FullStorageLock> current_lock_ = std::nullopt;
};

struct ItemCacheMap {
  std::map<AddressAndKey, ItemCache> local_map_;

  void commit_child_map(ItemCacheMap &child_map);
};

} // namespace hogd
