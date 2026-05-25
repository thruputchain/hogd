#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>

#include "contract.hh"
#include "hogd/base/string.h"

namespace hogd {

inline std::string
hex_string(std::span<const std::byte> bytes)
{
  return std::format("0x{}", encode_hex(bytes));
}

template<std::size_t N>
inline std::string
hex_string(std::array<std::uint8_t, N> const &bytes)
{
  return hex_string(std::as_bytes(std::span(bytes)));
}

inline std::string
entrypoint_hex(ContractEntrypointID const &entrypoint)
{
  return hex_string(std::as_bytes(std::span(entrypoint)));
}

} // namespace hogd
