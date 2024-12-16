#include "hogd/hogexec/runtime/item_cache.h"

#include <cassert>
#include <concepts>
#include <cstddef>
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

} // namespace

bool
CachedItem::exists() const
{
  return get_object_type().has_value();
}

std::optional<StorageObjectType>
CachedItem::get_object_type() const
{
  if (base_object_) {
    return base_object_->type();
  }

  return std::visit(
      [](auto const &v) {
        return std::remove_reference_t<decltype(v)>::kObjectType;
      },
      delta_args_);
}

SyscallStatus<void>
CachedItem::type_guard(StorageObjectType expected_type) const
{
  auto type = get_object_type();

  if (type) {
    if (*type != expected_type) {
      return SyscallError{SyscallErrorCode::kStorageObjectTypeMismatch};
    }
  }

  return {};
}

SyscallStatus<Bytestring>
CachedItem::bytestring_get() const
{
  auto type_guard_res = type_guard(StorageObjectType::kBytestring);

  if (!type_guard_res) {
    return {type_guard_res.error()};
  }

  if (const auto *args = get_bytestring_args()) {
    return args->set_bytestring_arg_;
  }

  if (base_object_) {
    return base_object_->bytestring();
  }

  return SyscallError{SyscallErrorCode::kStorageKeyNExist};
}

SyscallStatus<void>
CachedItem::bytestring_set(std::span<const std::byte> input_bytestring)
{
  auto type_guard_res = type_guard(StorageObjectType::kBytestring);

  if (!type_guard_res) {
    return {type_guard_res.error()};
  }

  if (input_bytestring.size() > kBytestringMaxLen) {
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }

  std::span<const uint8_t> reinterpreted_input = {
      reinterpret_cast<const uint8_t *>(input_bytestring.data()),
      input_bytestring.size()};

  delta_args_.emplace<BytestringCacheType>(
      BytestringCacheType{.set_bytestring_arg_ = {reinterpreted_input.begin(),
                                                  reinterpreted_input.end()}});

  return {};
}

SyscallStatus<int128_t>
CachedItem::nnint_get_base_value() const
{
  auto type_guard_res = type_guard(StorageObjectType::kNonnegativeInt);

  if (!type_guard_res) {
    return {type_guard_res.error()};
  }

  if (const auto *args = get_nnint_args()) {
    return args->nnint128_set_arg_;
  }

  if (base_object_) {
    return from_xdr(base_object_->nnint());
  }

  return SyscallError{SyscallErrorCode::kStorageKeyNExist};
}

SyscallStatus<int128_t>
CachedItem::nnint_get_value() const
{
  auto type_guard_res = type_guard(StorageObjectType::kNonnegativeInt);

  if (!type_guard_res) {
    return {type_guard_res.error()};
  }

  if (const auto *args = get_nnint_args()) {
    int128_t result;

    if (!integer_add_overflow(args->nnint128_set_arg_, args->nnint128_add_sum_,
                              &result)) {
      return result;
    }
    // Should be impossible for nnint128_add_sum_ to be less than 0 if we
    // overflow
    assert(args->nnint128_add_sum_ > 0);

    return std::numeric_limits<int128_t>::max();
  }

  if (base_object_) {
    return from_xdr(base_object_->nnint());
  }

  return SyscallError{SyscallErrorCode::kStorageKeyNExist};
}

namespace {

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

SyscallStatus<void>
CachedItem::nnint_set_add(int128_t set_value, int128_t add_value)
{
  auto type_guard_res = type_guard(StorageObjectType::kNonnegativeInt);

  if (!type_guard_res) {
    return {type_guard_res.error()};
  }

  int128_t prev_add_sum = 0;

  if (auto *args = get_nnint_args()) {
    if (args->nnint128_set_arg_ != set_value) {
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
    }
    prev_add_sum = args->nnint128_add_sum_;
  }

  int128_t new_add_value = capped_add(add_value, prev_add_sum);

  if (new_add_value < 0) {
    if (set_value < 0) {
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
    }
    if (set_value + new_add_value < 0) {
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
    }
  }

  if (auto *args = get_nnint_args()) {
    args->nnint128_add_sum_ = new_add_value;
  }
  else {
    delta_args_.emplace<NNInt128CacheType>(
        NNInt128CacheType{.nnint128_set_arg_ = set_value,
                          .nnint128_add_sum_ = new_add_value});
  }

  // TODO(geoff): Consider whether we should have add_value as int256,
  // so that sequences like add(INT128_MAX), add(INT128_MAX), add(INT128_MIN)
  // get handled in a more sensible way.

  return {};
}

std::optional<StorageDelta>
CachedItem::get_delta() const
{
  return std::visit([](auto const &v) { return v.get_delta(); }, delta_args_);
}

std::optional<StorageDelta>
CachedItem::BytestringCacheType::get_delta() const
{
  return make_bytestring_set(set_bytestring_arg_);
}

std::optional<StorageDelta>
CachedItem::NNInt128CacheType::get_delta() const
{
  return make_nnint128_set_add(nnint128_set_arg_, nnint128_add_sum_);
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
