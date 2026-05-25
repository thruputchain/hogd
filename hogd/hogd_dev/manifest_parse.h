// -*-C++-*-

#pragma once

#include <cstdint>
#include <string_view>

namespace hogd {

std::uint64_t parse_u64(std::string_view value, std::string_view name);

} // namespace hogd
