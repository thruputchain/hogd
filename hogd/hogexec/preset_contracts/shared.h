#pragma once

#include <string_view>

#include "types.hh"

namespace hogd {

MethodID sdk_method_id(std::string_view trait_name,
                       std::string_view method_name);

} // namespace hogd
