#pragma once

#include <string_view>

#include "contract.hh"
#include "hogd/base/types.h"
#include "types.hh"

namespace hogd {

// Common utilities for building preset contracts

// Get the method ID for a given method,
// at which contracts using the SDK will expect to find it.
MethodID sdk_method_id(std::string_view identifier_name,
                       std::string_view identifier_version, uint16_t method_id);
MethodID sdk_method_id(ContractEntrypointID entry_point_id);

} // namespace hogd
