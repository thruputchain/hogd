#include "hogd/hogexec/runtime/storage_proxy.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "storage_object.hh"
#include "syscall.hh"
#include "types.hh"

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
  out.nnint64() = NonnegativeInt64SetAddArgs{.set_value = set, .add_value = add};
  return out;
}

} // namespace

void
StorageProxy::delete_last()
{
  is_deleted_last_ = true;
}

bool
StorageProxy::exists() const
{
  return get_object_type().has_value();
}

std::optional<StorageObjectType>
StorageProxy::get_object_type() const
{
  if (base_object_) {
    return base_object_->type();
  }
  if (!!set_bytestring_arg_) {
    return StorageObjectType::kBytestring;
  }
  return std::nullopt;
}

SyscallStatus<void>
StorageProxy::type_guard(StorageObjectType expected_type) const
{
  if (base_object_ && (base_object_->type() != expected_type)) {
    return SyscallError{SyscallErrorCode::kStorageObjectTypeMismatch};
  }

  if (active_type_ && *active_type_ != expected_type) {
    return SyscallError{SyscallErrorCode::kStorageObjectTypeMismatch};
  }

  return {};
}

SyscallStatus<Bytestring>
StorageProxy::get_bytestring() const
{

  auto type_guard_res = type_guard(StorageObjectType::kBytestring);

  if (!type_guard_res) {
    return {type_guard_res.error()};
  }

  if (set_bytestring_arg_) {
    return *set_bytestring_arg_;
  }

  if (base_object_) {
    return base_object_->bytestring();
  }

  return SyscallError{SyscallErrorCode::kStorageKeyNExist};
}

SyscallStatus<uint64_t>
StorageProxy::get_bytestring_len() const
{
  return get_bytestring().and_then(
      [](Bytestring const &bstring) -> SyscallStatus<uint64_t> {
        return bstring.size();
      });
}

SyscallStatus<void>
StorageProxy::set_bytestring(std::span<const std::byte> input_bytestring)
{
  auto type_guard_res = type_guard(StorageObjectType::kBytestring);

  if (!type_guard_res) {
    return {type_guard_res.error()};
  }

  if (input_bytestring.size() > kBytestringMaxLen) {
    return SyscallError{SyscallErrorCode::kStringTooLong};
  }

  std::span<const uint8_t> reinterpreted_input = {
      reinterpret_cast<const uint8_t *>(input_bytestring.data()),
      input_bytestring.size()};

  set_bytestring_arg_.emplace(reinterpreted_input.begin(),
                              reinterpreted_input.end());

  active_type_ = StorageObjectType::kBytestring;
  return {};
}

SyscallStatus<int64_t>
StorageProxy::get_nnint64_base_value() const
{
  auto type_guard_res = type_guard(StorageObjectType::kNonnegativeInt64);

  if (!type_guard_res) {
    return {type_guard_res.error()};
  }

  if (set_nnint64_arg_) {
    return *set_nnint64_arg_;
  }

  if (base_object_) {
    return base_object_->int64();
  }

  return SyscallError{SyscallErrorCode::kStorageKeyNExist};
}

SyscallStatus<int64_t>
StorageProxy::get_nnint64_value() const
{
  auto base_value = get_nnint64_base_value();

  if (!base_value) {
    return {base_value.error()};
  }

  int64_t result;

  if (!__builtin_add_overflow(*base_value, nnint64_add_sum_, &result)) {
    return result;
  }

  // Should be impossible for nnint64_add_sum_ to be less than 0 if we overflow
  assert(nnint64_add_sum_ > 0);

  return INT64_MAX;
}

namespace {
int64_t
capped_add(int64_t a, int64_t b)
{
  int64_t out;
  if (__builtin_add_overflow(a, b, &out)) {
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
StorageProxy::nnint64_set_add(int64_t set_value, int64_t add_value)
{
  auto type_guard_res = type_guard(StorageObjectType::kNonnegativeInt64);

  if (!type_guard_res) {
    return {type_guard_res.error()};
  }

  if (set_nnint64_arg_) {
    if (*set_nnint64_arg_ != set_value) {
      return SyscallError{SyscallErrorCode::kInvalidSetAdd};
    }
  }

  int64_t new_add_value = capped_add(add_value, nnint64_add_sum_);

  if (new_add_value < 0) {
    if (set_value < 0) {
      return SyscallError{SyscallErrorCode::kInvalidSetAdd};
    }
    if (set_value + new_add_value < 0) {
      return SyscallError{SyscallErrorCode::kInvalidSetAdd};
    }
  }

  // TODO(geoff): Consider whether we should have add_value as int128,
  // so that sequences like add(INT64_MAX), add(INT64_MAX), add(INT64_MIN) get
  // handled in a more sensible way.
  set_nnint64_arg_ = set_value;
  nnint64_add_sum_ = new_add_value;

  active_type_ = StorageObjectType::kNonnegativeInt64;

  return {};
}

x23::Vec<StorageDelta>
StorageProxy::get_deltas() const
{
  x23::Vec<StorageDelta> out;

  if (is_deleted_last_) {
    out.push_back(make_delete_last());
  }

  if (set_bytestring_arg_) {
    out.push_back(make_bytestring_set(*set_bytestring_arg_));
  }

  if (set_nnint64_arg_) {
    out.push_back(make_nnint64_set_add(*set_nnint64_arg_, nnint64_add_sum_));
  }

  // TODO(geoff) others
  return out;
}

} // namespace hogd
