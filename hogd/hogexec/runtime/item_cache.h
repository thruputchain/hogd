#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <variant>

#include "hogd/hogexec/runtime/status.h"
#include "storage_object.hh"
#include "types.hh"

namespace hogd {

class ItemCache {
public:
  ItemCache(std::optional<StorageObject> obj) : base_object_(std::move(obj)) {}

  bool exists() const;
  std::optional<StorageObjectType> get_object_type() const;

  void delete_last();

  SyscallStatus<Bytestring> get_bytestring() const;
  SyscallStatus<uint32_t> get_bytestring_len() const;
  SyscallStatus<void>
  set_bytestring(std::span<const std::byte> input_bytestring);

  SyscallStatus<int128_t> get_nnint128_base_value() const;
  SyscallStatus<int128_t> get_nnint128_value() const;
  SyscallStatus<void> nnint128_set_add(int128_t set_value, int128_t add_value);

  x23::Vec<StorageDelta> get_deltas() const;

private:
  SyscallStatus<void> type_guard(StorageObjectType expected_type) const;

  struct BytestringCacheType {
    Bytestring set_bytestring_arg_;

    x23::Vec<StorageDelta> get_deltas() const;

    constexpr static std::optional<StorageObjectType> kObjectType =
        StorageObjectType::kBytestring;
  };

  struct NNInt128CacheType {
    int128_t nnint128_set_arg_ = 0;
    int128_t nnint128_add_sum_ = 0;

    x23::Vec<StorageDelta> get_deltas() const;

    constexpr static std::optional<StorageObjectType> kObjectType =
        StorageObjectType::kNonnegativeInt128;
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

  auto *get_nnint128_args(this auto &self)
  {
    if (std::holds_alternative<NNInt128CacheType>(self.delta_args_)) {
      return &std::get<NNInt128CacheType>(self.delta_args_);
    }
    return static_cast<decltype(&std::get<NNInt128CacheType>(
        self.delta_args_))>(nullptr);
  }

  std::optional<StorageObject> base_object_;
  std::variant<UnsetType, BytestringCacheType, NNInt128CacheType> delta_args_ =
      UnsetType{};

  bool is_deleted_last_ = false;
};

struct ItemCacheMap {
  std::map<AddressAndKey, ItemCache> local_map_;

  void commit_child_map(ItemCacheMap &child_map);
};

} // namespace hogd
