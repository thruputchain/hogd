#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <variant>

#include "hogd/base/compiler.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/status.h"
#include "storage_object.hh"
#include "types.hh"

namespace hogd {

// store relevant state when object contains bytestring
class BytestringItem {
public:
  BytestringItem(const StorageObject *base);

  constexpr static std::optional<StorageObjectType> kObjectType =
      StorageObjectType::kBytestring;
  bool has_modification() const { return has_modification_; }

  SyscallStatus<void> set(std::span<const std::byte> arg);
  SyscallStatus<std::reference_wrapper<const Bytestring>> get() const;

  std::optional<StorageDelta> get_delta() const;

private:
  bool has_modification_ = false;
  std::optional<Bytestring> set_arg_;
};

// store relevant state when object contains nnint
class NNIntItem {
public:
  NNIntItem(const StorageObject *base);

  constexpr static std::optional<StorageObjectType> kObjectType =
      StorageObjectType::kNonnegativeInt;
  bool has_modification() const { return has_modification_; }

  SyscallStatus<int128_t> get_base() const;
  SyscallStatus<int128_t> get() const;
  SyscallStatus<void> set_add(int128_t set_value, int128_t add_value);

  std::optional<StorageDelta> get_delta() const;

private:
  bool has_modification_ = false;
  std::optional<int128_t> base_object_value_;
  int128_t add_sum_ = 0;
  int128_t set_arg_ = 0;
};

// object is in an as-yet-unmodified state
struct UnsetItem {
  constexpr static std::optional<StorageObjectType> kObjectType = std::nullopt;
  constexpr static bool has_modification() { return false; }

  constexpr static std::optional<StorageDelta> get_delta()
  {
    return std::nullopt;
  }
};

// object has been deleted
struct DeletedItem {
  constexpr static std::optional<StorageObjectType> kObjectType = std::nullopt;
  constexpr static bool has_modification() { return true; }

  static std::optional<StorageDelta> get_delta();
};

// Maintain the modifications of one InvocationFrame to one address.
// Provides for a transaction a locally consistent view of a storage item,
// with modifications to the item up to that point in the transaction applied to
// it.
//
// Methods marked `const` are read-only, so accessing them does not change
// the internal state of the CachedItem.
// For example, if base_object_ is nullopt, a call to bytestring_get() does not
// then force the CachedItem into a Bytestring type.  A subsequent call to
// nnint_set_add could succeed. If a CachedItem is only ever accessed via const
// methods, no StorageDelta will be produced.
class CachedItem {
public:
  CachedItem(const StorageObject *obj) : base_object_(obj) {}

  std::optional<StorageObjectType> get_object_type() const;
  SyscallStatus<std::reference_wrapper<BytestringItem>> bytestring();
  SyscallStatus<std::reference_wrapper<NNIntItem>> nnint();
  void delete_object();

  std::optional<StorageDelta> get_delta() const;

private:
  const StorageObject *base_object_;
  std::variant<UnsetItem, DeletedItem, BytestringItem, NNIntItem> delta_args_ =
      UnsetItem{};

  SyscallStatus<void>
  type_guard(std::optional<StorageObjectType> expected_type) const;
  void debug_assert_no_modifications() const;

  template<typename ItemType>
  SyscallStatus<std::reference_wrapper<ItemType>> get_item()
  {
    auto type_guard_res = type_guard(ItemType::kObjectType);
    if (!type_guard_res) {
      return {type_guard_res.error()};
    }

    if (std::holds_alternative<ItemType>(delta_args_)) {
      return std::get<ItemType>(delta_args_);
    }
    if constexpr (is_debug_build()) {
      debug_assert_no_modifications();
    }
    delta_args_.emplace<ItemType>(base_object_);

    return std::get<ItemType>(delta_args_);
  }
};

// Maintains a map of the items modified within one CallFrame.
struct ItemCacheMap {
  std::unordered_map<AddressAndKey, CachedItem> local_map_;

  // Absorb the modifications created during a child CallFrame.
  // Overwrites entries in local_map_ as necessary.
  // Resolution of cached item entries is performed in the syscall handler.
  void commit_child_map(ItemCacheMap &child_map);
};

} // namespace hogd
