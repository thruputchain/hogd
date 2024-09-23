#include "hogd/hogexec/runtime/storage_proxy.h"

#include "hogd/hogexec/runtime/status.h"

namespace hogd {

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
    return SyscallError{SyscallErrorCode::kBytestringTooLong};
  }

  std::span<const uint8_t> reinterpreted_input = {
      reinterpret_cast<const uint8_t *>(input_bytestring.data()),
      input_bytestring.size()};

  set_bytestring_arg_.emplace(reinterpreted_input.begin(),
                              reinterpreted_input.end());

  return {};
}

} // namespace hogd
