#include "hogd/devnode/manifest_parse.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#include "hogd/base/cli.h"

namespace hogd {

std::optional<std::uint64_t>
try_parse_u64(std::string_view value)
{
  // JSON integers are represented as int64_t in hogd::json, so decimal strings
  // use the same upper bound for round-trip compatibility. try_parse_integer
  // already rejects empty input, a leading sign, leading whitespace, and
  // trailing characters, so only the int64_t ceiling is left to enforce.
  std::optional<std::uint64_t> parsed = try_parse_integer<std::uint64_t>(value);
  if (!parsed || *parsed > static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max())) {
    return std::nullopt;
  }
  return parsed;
}

} // namespace hogd
