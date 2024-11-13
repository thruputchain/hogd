#include "hogd/hogexec/runtime/item_cache.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "storage_object.hh"
#include "syscall.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

namespace {

StorageDelta
make_delete_last()
{
  StorageDelta out;
  out.type(StorageDeltaType::kDeleteLast);
  return out;
}

StorageDelta
make_bytestring_set(Bytestring const &string)
{
  StorageDelta out;
  out.type(StorageDeltaType::kBytestringSet);
  out.bytestring() = string;
  return out;
}

StorageDelta
make_nnint64_set_add(int64_t set, int64_t add)
{
  StorageDelta out;
  out.type(StorageDeltaType::kNonnegativeInt64SetAdd);
  out.nnint64() =
      NonnegativeInt64SetAddArgs{.set_value = set, .add_value = add};
  return out;
}

} // namespace

void
ItemCache::delete_last()
{
  is_deleted_last_ = true;
}

bool
ItemCache::exists() const
{
  return get_object_type().has_value();
}

std::optional<StorageObjectType>
ItemCache::get_object_type() const
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
ItemCache::type_guard(StorageObjectType expected_type) const
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
ItemCache::get_bytestring() const
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

SyscallStatus<uint64_t>
ItemCache::get_bytestring_len() const
{
  return get_bytestring().and_then(
      [](Bytestring const &bstring) -> SyscallStatus<uint64_t> {
        return bstring.size();
      });
}

SyscallStatus<void>
ItemCache::set_bytestring(std::span<const std::byte> input_bytestring)
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

SyscallStatus<int64_t>
ItemCache::get_nnint64_base_value() const
{
  auto type_guard_res = type_guard(StorageObjectType::kNonnegativeInt64);

  if (!type_guard_res) {
    return {type_guard_res.error()};
  }

  if (const auto *args = get_nnint64_args()) {
    return args->nnint64_set_arg_;
  }

  if (base_object_) {
    return base_object_->int64();
  }

  return SyscallError{SyscallErrorCode::kStorageKeyNExist};
}

SyscallStatus<int64_t>
ItemCache::get_nnint64_value() const
{
  auto type_guard_res = type_guard(StorageObjectType::kNonnegativeInt64);

  if (!type_guard_res) {
    return {type_guard_res.error()};
  }

  if (const auto *args = get_nnint64_args()) {
    int64_t result;

    if (!integer_add_overflow(args->nnint64_set_arg_, args->nnint64_add_sum_,
                              &result)) {
      return result;
    }
    // Should be impossible for nnint64_add_sum_ to be less than 0 if we
    // overflow
    assert(args->nnint64_add_sum_ > 0);

    return INT64_MAX;
  }

  if (base_object_) {
    return base_object_->int64();
  }

  return SyscallError{SyscallErrorCode::kStorageKeyNExist};
}

namespace {
int64_t
capped_add(int64_t a, int64_t b)
{
  int64_t out;
  if (integer_add_overflow(a, b, &out)) {
    if (a > 0) {
      assert(b > 0);
      return INT64_MAX;
    }
    // else a < 0
    assert(a < 0);
    assert(b < 0);
    return INT64_MIN;
  }
  return out;
}
} // namespace

SyscallStatus<void>
ItemCache::nnint64_set_add(int64_t set_value, int64_t add_value)
{
  auto type_guard_res = type_guard(StorageObjectType::kNonnegativeInt64);

  if (!type_guard_res) {
    return {type_guard_res.error()};
  }

  int64_t prev_add_sum = 0;

  if (auto *args = get_nnint64_args()) {
    if (args->nnint64_set_arg_ != set_value) {
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
    }
    prev_add_sum = args->nnint64_add_sum_;
  }

  int64_t new_add_value = capped_add(add_value, prev_add_sum);

  if (new_add_value < 0) {
    if (set_value < 0) {
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
    }
    if (set_value + new_add_value < 0) {
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
    }
  }

  if (auto *args = get_nnint64_args()) {
    args->nnint64_add_sum_ = new_add_value;
  }
  else {
    delta_args_.emplace<NNInt64CacheType>(
        NNInt64CacheType{.nnint64_set_arg_ = set_value,
                         .nnint64_add_sum_ = new_add_value});
  }

  // TODO(geoff): Consider whether we should have add_value as int128,
  // so that sequences like add(INT64_MAX), add(INT64_MAX), add(INT64_MIN) get
  // handled in a more sensible way.

  return {};
}

x23::Vec<StorageDelta>
ItemCache::get_deltas() const
{
  x23::Vec<StorageDelta> out =
      std::visit([](auto const &v) { return v.get_deltas(); }, delta_args_);

  if (is_deleted_last_) {
    out.push_back(make_delete_last());
  }

  return out;
}

x23::Vec<StorageDelta>
ItemCache::BytestringCacheType::get_deltas() const
{
  return {make_bytestring_set(set_bytestring_arg_)};
}

x23::Vec<StorageDelta>
ItemCache::NNInt64CacheType::get_deltas() const
{
  return {make_nnint64_set_add(nnint64_set_arg_, nnint64_add_sum_)};
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
