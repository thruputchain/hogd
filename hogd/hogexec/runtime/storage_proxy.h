#pragma once

#include <cstdint>
#include <optional>

#include "hogd/hogexec/runtime/status.h"
#include "storage_object.hh"
#include "types.hh"

namespace hogd {

class StorageProxy {
public:
  StorageProxy(std::optional<StorageObject> obj) : base_object_(obj) {}

  void delete_last();

  bool exists() const;
  std::optional<StorageObjectType> get_object_type() const;

  SyscallStatus<Bytestring> get_bytestring() const;
  SyscallStatus<uint64_t> get_bytestring_len() const;
  SyscallStatus<void>
  set_bytestring(std::span<const std::byte> input_bytestring);

private:
  SyscallStatus<void> type_guard(StorageObjectType expected_type) const;

  std::optional<StorageObject> base_object_;
  std::optional<Bytestring> set_bytestring_arg_;
  bool is_deleted_last_ = false;
};

} // namespace hogd
