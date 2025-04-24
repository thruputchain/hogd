#include "hogd/hogexec/runtime/item_view.h"

#include <optional>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/status.h"
#include "storage_item.hh"
#include "syscall.hh"

namespace hogd {

namespace {

StorageDelta
make_bytestring_set(Bytestring const &string)
{
  StorageDelta out(StorageDeltaType::kBytestringSet);
  out.bytestring() = string;
  return out;
}

StorageDelta
make_nnint128_set_add(int128_t set, int128_t add)
{
  StorageDelta out(StorageDeltaType::kNonnegativeIntSetAdd);
  out.nnint() = NonnegativeInt128SetAddArgs{.set_value = to_xdr(set),
                                            .add_value = to_xdr(add)};

  return out;
}

} // namespace

std::optional<StorageItemType>
ItemView::get_item_type() const
{
  return std::visit(
      [](auto const &v) -> std::optional<StorageItemType> {
        return v.get_item_type();
      },
      delta_args_);
}

std::optional<StorageDelta>
ItemView::get_delta() const
{
  return std::visit(
      [](auto const &v) -> std::optional<StorageDelta> {
        return v.get_delta();
      },
      delta_args_);
}

SyscallStatus<void>
ItemView::can_accept_type(std::optional<StorageItemType> item_type) const
{
  if (!item_type) {
    // Writing a deletion; this is always allowed
    return {};
  }

  auto current_type = get_item_type();
  if (!current_type) {
    // Item is in deleted state; therefore it is allowed to write anything
    return {};
  }

  if (*current_type != *item_type) {
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }

  return {};
}

StorageDelta
BytestringItem::get_delta() const
{
  return make_bytestring_set(bytestring_);
}

StorageDelta
NNIntItem::get_delta() const
{
  return make_nnint128_set_add(base_value_, add_value_);
}

SyscallStatus<void>
BytestringItem::validate() const
{
  if (bytestring_.size() > kBytestringMaxLen)
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};

  return {};
}

SyscallStatus<void>
NNIntItem::validate() const
{
  if (add_value_ >= 0) {
    // When add_value_ is nonnegative, the storage delta produced
    // by this item is always valid.
    return {};
  }

  if (base_value_ < 0) {
    // cannot subtract from a value that is already negative
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }

  if (base_value_ >= 0) {
    // base_value_ + add_value_ cannot overflow here,
    // because base_value_ and add_value_ have opposite signs
    if (base_value_ + add_value_ < 0) {
      // This would be always rejected by the delta handling logic
      // in the storage server.  Putting this check here
      // may simplify some downstream logic.
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
    }
  }

  return {};
}

std::optional<StorageItemType>
UnmodifiedItem::get_item_type() const
{
  if (!base_item_) {
    // no base object exists in the snapshot,
    // so object has no type
    return std::nullopt;
  }
  return base_item_->type();
}

} // namespace hogd
