// -*-C++-*-

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

#include "hogd/version.h"
#include "network.hh"

namespace hogd {

// The protocol token is a fixed-width array on the wire, but the same bytes
// are printable ASCII and travel as text through runtime manifests, log
// lines, and indexer outbox filenames. hogd/version.h builds the token
// without the generated headers, so this is where the two widths meet.
static_assert(
    std::is_same_v<ProtocolCommit,
                   std::array<std::uint8_t, version::kHogdProtocolLen>>,
    "bug: ProtocolCommit width must match version::kHogdProtocolLen");

/// The protocol token as printable text. The view borrows [protocol], so it
/// lives exactly as long as the value passed in.
inline std::string_view
protocol_text(const ProtocolCommit &protocol)
{
  return {reinterpret_cast<const char *>(protocol.data()), protocol.size()};
}

/// A token built from text, padded or truncated to the fixed width the same
/// way hogd derives its own (see version::kHogdProtocolCommit). Round-trips
/// with [protocol_text] for any text that already fills the array.
inline ProtocolCommit
protocol_from_text(std::string_view text)
{
  ProtocolCommit protocol;
  protocol.fill('_');
  std::copy_n(text.begin(), std::min(text.size(), protocol.size()),
              protocol.begin());
  return protocol;
}

} // namespace hogd
