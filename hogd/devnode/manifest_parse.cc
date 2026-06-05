#include "hogd/devnode/manifest_parse.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hogd {

std::uint64_t
parse_u64(std::string_view value, std::string_view name)
{
  // JSON integers are represented as int64_t in hogd::json, so decimal strings
  // use the same upper bound for round-trip compatibility.
  if (value.empty() || value.front() == '+' || value.front() == '-' ||
      static_cast<unsigned char>(value.front()) <= ' ') {
    throw std::runtime_error(std::format("Invalid {}: {}", name, value));
  }
  std::size_t pos = 0;
  std::uint64_t parsed = 0;
  try {
    parsed = std::stoull(std::string(value), &pos, 10);
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
