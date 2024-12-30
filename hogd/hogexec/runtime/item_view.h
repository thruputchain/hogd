#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <variant>

#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/status.h"
#include "storage_object.hh"
#include "types.hh"

namespace hogd {

// Representing the internal state of a bytestring item.
class BytestringItem {
public:
  StorageDelta get_delta() const;
  SyscallStatus<void> validate() const;

  constexpr static StorageObjectType get_object_type()
  {
    return StorageObjectType::kBytestring;
  }

  Bytestring bytestring_;
};

// State of an item that is a nonnegative int
class NNIntItem {
public:
  StorageDelta get_delta() const;
  SyscallStatus<void> validate() const;

  constexpr static StorageObjectType get_object_type()
  {
    return StorageObjectType::kNonnegativeInt;
  }

  int128_t base_value_;
  int128_t add_value_;
};

// Represent state of item that has been deleted.
class DeletedItem {

public:
  constexpr static StorageDelta get_delta()
  {
    return StorageDelta(StorageDeltaType::kDelete);
  }

  constexpr static SyscallStatus<void> validate() { return {}; }

  constexpr static std::optional<StorageObjectType> get_object_type()
  {
    return std::nullopt;
  }
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

  std::optional<StorageObjectType> get_object_type() const;

  const StorageObject *const base_object_;
};

// Maintain the modifications of one InvocationFrame to one address.
// Provides for a transaction a locally consistent view of a storage item,
// with modifications to the item up to that point in the transaction applied to
// it.
class ItemView {
public:
  ItemView(const StorageObject *base_object)
    : delta_args_(UnmodifiedItem{.base_object_ = base_object})
  {}

  std::optional<StorageObjectType> get_object_type() const;

  template<typename ItemType> auto *get(this auto &self)
  {
    if (std::holds_alternative<ItemType>(self.delta_args_)) {
      return &std::get<ItemType>(self.delta_args_);
    }
    return static_cast<decltype(&std::get<ItemType>(self.delta_args_))>(
        nullptr);
  }

  template<typename ItemType> SyscallStatus<void> set(ItemType const &type)
  {
    auto validate_result = type.validate();
    if (!validate_result) {
      // Validation check asserts that the written item type is well-formed.
      return validate_result;
    }
    delta_args_ = type;
    return {};
  }

  std::optional<StorageDelta> get_delta() const;

private:
  std::variant<UnmodifiedItem, DeletedItem, BytestringItem, NNIntItem>
      delta_args_;
};

} // namespace hogd
