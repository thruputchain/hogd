#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

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

inline std::uint64_t
parse_u64(std::string const &value, std::string_view name)
{
  if (value.empty() || value.front() == '+' || value.front() == '-' ||
      static_cast<unsigned char>(value.front()) <= ' ') {
    throw std::runtime_error(std::format("Invalid {}: {}", name, value));
  }
  std::size_t pos = 0;
  std::uint64_t parsed = 0;
  try {
    parsed = std::stoull(value, &pos, 10);
  } catch (const std::exception &) {
    throw std::runtime_error(std::format("Invalid {}: {}", name, value));
  }
  if (pos != value.size() ||
      parsed > static_cast<std::uint64_t>(
                   std::numeric_limits<std::int64_t>::max())) {
    throw std::runtime_error(std::format("Invalid {}: {}", name, value));
  }
  return parsed;
}

} // namespace hogd
