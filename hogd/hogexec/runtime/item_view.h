#pragma once

#include <optional>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/status.h"
#include "storage_item.hh"

namespace hogd {

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

  constexpr static StorageItemType get_item_type()
  {
    return StorageItemType::kBytestring;
  }

  Bytestring bytestring_;
};

// State of an item that is a nonnegative int
class NNIntItem {
public:
  StorageDelta get_delta() const;
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
  constexpr static StorageDelta get_delta()
  {
    return StorageDelta(StorageDeltaType::kDelete);
  }

  constexpr static SyscallStatus<void> validate() { return {}; }

  constexpr static std::optional<StorageItemType> get_item_type()
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

  std::optional<StorageItemType> get_item_type() const;

  const StorageItem *const base_item_;
};

// Maintain the modifications of one InvocationFrame to one address.
// Provides for a transaction a locally consistent view of a storage item,
// with modifications to the item up to that point in the transaction applied to
// it.
class ItemView {
public:
  ItemView(const StorageItem *base_object)
    : delta_args_(UnmodifiedItem{.base_item_ = base_object})
  {}

  std::optional<StorageItemType> get_item_type() const;

  template<typename ItemType> const auto *get()
  {
    return std::holds_alternative<ItemType>(delta_args_)
               ? &std::get<ItemType>(delta_args_)
               : nullptr;
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
