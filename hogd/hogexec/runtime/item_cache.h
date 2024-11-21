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

private:
  SyscallStatus<void> type_guard(StorageObjectType expected_type) const;

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
    constexpr static x23::Vec<StorageDelta> get_deltas() {
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

  bool is_deleted_last_ = false;
};

struct ItemCacheMap {
  std::map<AddressAndKey, ItemCache> local_map_;

  void commit_child_map(ItemCacheMap &child_map);
};

} // namespace hogd
