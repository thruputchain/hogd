#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "hogd/base/base58.h"
#include "types.hh"

namespace hogd {

inline std::optional<NetworkHash>
parse_network_hash(std::string_view text)
{
  return decode_base58_fixed<32>(text);
}

inline std::string
format_network_hash(const NetworkHash &hash)
{
  return encode_base58(hash);
}

} // namespace hogd
