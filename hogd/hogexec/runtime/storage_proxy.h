#pragma once

#include <optional>
#include <cstdint>

#include "types.hh"
#include "storage_object.hh"
#include "hogd/hogexec/runtime/status.h"

namespace hogd {

class StorageProxy {
public:

	StorageProxy(std::optional<StorageObject> obj)
		: base_object_(obj)
		{}

	void delete_last();

	bool exists() const;

	SyscallStatus<Bytestring> get_bytestring() const;

	SyscallStatus<uint64_t> get_bytestring_len() const;

private:

	SyscallStatus<void> type_guard(StorageObjectType expected_type) const;

	std::optional<StorageObject> base_object_;
	std::optional<Bytestring> set_bytestring_arg_;
	bool is_deleted_last_ = false;

};

}