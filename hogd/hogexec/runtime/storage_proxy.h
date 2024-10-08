#pragma once

#include <cstdint>
#include <optional>

#include "hogd/hogexec/runtime/status.h"
#include "storage_object.hh"
#include "types.hh"

namespace hogd {

class StorageProxy {
public:
  StorageProxy(std::optional<StorageObject> obj) : base_object_(std::move(obj))
  {}

  void delete_last();

  bool exists() const;
  std::optional<StorageObjectType> get_object_type() const;

  SyscallStatus<Bytestring> get_bytestring() const;
  SyscallStatus<uint64_t> get_bytestring_len() const;
  SyscallStatus<void>
  set_bytestring(std::span<const std::byte> input_bytestring);

  SyscallStatus<int64_t> get_nnint64_base_value() const;
  SyscallStatus<int64_t> get_nnint64_value() const;
  SyscallStatus<void> nnint64_set_add(int64_t set_value, int64_t add_value);

private:
  SyscallStatus<void> type_guard(StorageObjectType expected_type) const;

  std::optional<StorageObject> base_object_;
  std::optional<StorageObjectType> active_type_ = std::nullopt;

  std::optional<Bytestring> set_bytestring_arg_ = std::nullopt;
  std::optional<int64_t> set_nnint64_arg_ = std::nullopt;
  int64_t nnint64_add_sum_ = 0;
  bool is_deleted_last_ = false;
};

} // namespace hogd
