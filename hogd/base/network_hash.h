#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "hogd/base/xdr_util.h"
#include "types.hh"

namespace hogd {

inline std::optional<NetworkHash>
parse_network_hash(std::string_view text)
{
  if (text.size() != NetworkHash{}.size() * 2) {
    return std::nullopt;
  }

  auto hex_value = [](char c) -> std::optional<uint8_t> {
    if (c >= '0' && c <= '9') {
      return static_cast<uint8_t>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
      return static_cast<uint8_t>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
      return static_cast<uint8_t>(c - 'A' + 10);
    }
    return std::nullopt;
  };

  NetworkHash hash{};
  for (size_t i = 0; i < hash.size(); ++i) {
    const std::optional<uint8_t> high = hex_value(text[i * 2]);
    const std::optional<uint8_t> low = hex_value(text[(i * 2) + 1]);
    if (!high || !low) {
      return std::nullopt;
    }
    hash[i] = static_cast<uint8_t>((*high << 4) | *low);
  }
  return hash;
}

inline std::string
format_network_hash(const NetworkHash &hash)
{
  return encode_hex(std::as_bytes(std::span(hash)));
}

} // namespace hogd
