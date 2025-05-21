#pragma once

#include <string_view>

#include "types.hh"

namespace hogd {

// Common utilities for building preset contracts

// Get the method ID for a given method,
// at which contracts using the SDK will expect to find it.
MethodID sdk_method_id(std::string_view trait_name,
                       std::string_view method_name);

} // namespace hogd
