// -*-C++-*-

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "event_cursor_internal.hh"

namespace hogd::cursor {

// Bumped any time `EventCursorPayload`'s wire shape changes.
// Cursors with a different schema version are rejected.
inline constexpr std::uint32_t kCursorVersion = 1;

// Hard cap on accepted cursor bytes.  The encoded form is well
// below this; the cap is a defensive guard against arbitrarily
// large client input.
inline constexpr std::size_t kMaxCursorBytes = 256;

// Serialize a cursor payload to its on-wire form.  The `schema_version`
// field of `payload` is overwritten with `kCursorVersion`.
[[nodiscard]] std::string encode(EventCursorPayload payload);

// Deserialize an opaque cursor blob.  Returns `nullopt` if the
// buffer is empty, oversized, malformed XDR, or carries a schema
// version other than `kCursorVersion`.
[[nodiscard]] std::optional<EventCursorPayload> decode(std::string_view bytes);

} // namespace hogd::cursor

namespace hogd {

// Limit clamping constants for get_events.  Limit of 0 is interpreted
// as "server default"; any value outside [1, kMax] is clamped.
inline constexpr std::uint32_t kEventsDefaultLimit = 50;
inline constexpr std::uint32_t kEventsMaxLimit = 200;

[[nodiscard]] inline std::uint32_t
clamp_events_limit(std::uint32_t requested)
{
  if (requested == 0) {
    return kEventsDefaultLimit;
  }
  if (requested > kEventsMaxLimit) {
    return kEventsMaxLimit;
  }
  return requested;
}

} // namespace hogd
