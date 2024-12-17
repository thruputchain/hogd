#include "hogd/hogexec/runtime/item_cache.h"

#include <cassert>
#include <concepts>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/status.h"
#include "storage_object.hh"
#include "syscall.hh"

namespace hogd {

namespace {

StorageDelta
make_bytestring_set(Bytestring const &string)
{
  StorageDelta out;
  out.type(StorageDeltaType::kBytestringSet);
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

template<std::integral T>
T
capped_add(T a, T b)
{
  T out;
  if (integer_add_overflow(a, b, &out)) {
    if (a > 0) {
      assert(b > 0);
      return std::numeric_limits<T>::max();
    }
    // else a < 0
    assert(a < 0);
    assert(b < 0);
    return std::numeric_limits<T>::min();
  }
  return out;
}

} // namespace

std::optional<StorageObjectType>
CachedItem::get_object_type() const
{
  auto active_type = std::visit(
      [](auto const &v) -> std::optional<StorageObjectType> {
        return v.has_modification()
                   ? std::remove_reference_t<decltype(v)>::kObjectType
                   : std::nullopt;
      },
      delta_args_);

  if (active_type) {
    // item cache has been actively modified during current
    // transaction.  Current type is whatever type was left by last write.
    return active_type;
  }

  if (base_object_) {
    // Item cache has not been actively modified during current
    // transaction, but snapshot value exists.
    return base_object_->type();
  }

  return std::nullopt;
}

SyscallStatus<void>
CachedItem::type_guard(std::optional<StorageObjectType> expected_type) const
{
  if (!expected_type) {
    return {};
  }

  auto type = get_object_type();

  if (type && *type != expected_type) {
    return SyscallError{SyscallErrorCode::kStorageObjectTypeMismatch};
  }

  return {};
}

BytestringItem::BytestringItem(const StorageObject *base)
{
  if (base) {
    assert(base->type() == StorageObjectType::kBytestring);
    set_arg_ = base->bytestring();
  }
}

NNIntItem::NNIntItem(const StorageObject *base)
{
  if (base) {
    assert(base->type() == StorageObjectType::kNonnegativeInt);
    base_object_value_ = from_xdr(base->nnint());
  }
}

SyscallStatus<void>
BytestringItem::set(std::span<const std::byte> arg)
{
  if (arg.size() > kBytestringMaxLen) {
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }

  std::span<const uint8_t> reinterpreted_input = {
      reinterpret_cast<const uint8_t *>(arg.data()), arg.size()};
  set_arg_ = {reinterpreted_input.begin(), reinterpreted_input.end()};
  has_modification_ = true;
  return {};
}

SyscallStatus<std::reference_wrapper<const Bytestring>>
BytestringItem::get() const
{
  if (set_arg_)
    return *set_arg_;
  return SyscallError{SyscallErrorCode::kStorageKeyNExist};
}

void
CachedItem::delete_object()
{
  delta_args_.emplace<DeletedItem>();
  base_object_ = nullptr;
}

SyscallStatus<std::reference_wrapper<BytestringItem>>
CachedItem::bytestring()
{
  return get_item<BytestringItem>();
}

SyscallStatus<std::reference_wrapper<NNIntItem>>
CachedItem::nnint()
{
  return get_item<NNIntItem>();
}

void
CachedItem::debug_assert_no_modifications() const
{
  std::visit([](auto const &v) { assert(!v.has_modification()); }, delta_args_);
}

SyscallStatus<int128_t>
NNIntItem::get_base() const
{
  if (has_modification_)
    return set_arg_;

  if (base_object_value_.has_value())
    return *base_object_value_;

  return SyscallError{SyscallErrorCode::kStorageKeyNExist};
}

SyscallStatus<int128_t>
NNIntItem::get() const
{
  if (!has_modification_) {
    // object wasn't actually modified during current transaction.
    // Must ignore local data structures, and default to snapshot value.
    return base_object_value_
               ? SyscallStatus<int128_t>(*base_object_value_)
               : SyscallError{SyscallErrorCode::kStorageKeyNExist};
  }

  int128_t result;
  if (!integer_add_overflow(set_arg_, add_sum_, &result))
    return result;

  // Should be impossible for nnint128_add_sum_ to be less than 0 if we
  // overflow
  assert(add_sum_ > 0);

  return std::numeric_limits<int128_t>::max();
}

SyscallStatus<void>
NNIntItem::set_add(int128_t set_value, int128_t add_value)
{
  if (has_modification_ && set_value != set_arg_)
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  
  int128_t new_add_value = capped_add(add_value, add_sum_);

  if (new_add_value < 0) {
    if (set_value < 0)
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};

    // cannot overflow
    if (set_value + new_add_value < 0)
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }

  add_sum_ = new_add_value;
  set_arg_ = set_value;
  has_modification_ = true;

  return {};
}

std::optional<StorageDelta>
BytestringItem::get_delta() const
{
  if (!has_modification_)
    return std::nullopt;
  
  assert(set_arg_.has_value());
  return make_bytestring_set(*set_arg_);
}

std::optional<StorageDelta>
NNIntItem::get_delta() const
{
  if (!has_modification_)
    return std::nullopt;

  return make_nnint128_set_add(set_arg_, add_sum_);
}

std::optional<StorageDelta>
DeletedItem::get_delta()
{
  StorageDelta delete_delta(StorageDeltaType::kDelete);
  return delete_delta;
}

std::optional<StorageDelta>
CachedItem::get_delta() const
{
  return std::visit(
      [](auto const &v) -> std::optional<StorageDelta> {
        return v.get_delta();
      },
      delta_args_);
}

void
ItemCacheMap::commit_child_map(ItemCacheMap &child_map)
{
  for (auto &[addr, item] : child_map.local_map_) {
    auto it = local_map_.find(addr);
    if (it == local_map_.end()) {
      local_map_.emplace(addr, std::move(item));
    }
    else {
      local_map_.at(addr) = std::move(item);
    }
  }
}

} // namespace hogd
