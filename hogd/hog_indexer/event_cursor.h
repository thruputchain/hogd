// -*-C++-*-

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "event_cursor_internal.hh"
#include "indexer.hh"
#include "types.hh"

namespace hogd::cursor {

// Bumped any time `EventCursorPayload`'s wire shape changes.
// Cursors with a different schema version are rejected.
inline constexpr std::uint32_t kCursorVersion = 3;

// Hard cap on accepted cursor bytes.  The encoded form is well
// below this; the cap is a defensive guard against arbitrarily
// large client input.
inline constexpr std::size_t kMaxCursorBytes = 256;

// Hash the event predicate after sorting/deduplicating OR sets and filters.
// Topic positions and filter grouping retain their meaning.
[[nodiscard]] SHA256Hash event_query_hash(EventFilterList const &filters);

// Bind root-address history to its requested account. Whole-chain cursors
// have no predicate and use a zero query hash with CursorTransactions.
[[nodiscard]] SHA256Hash address_query_hash(ContractAddress const &address);

// Serialize a cursor payload to its on-wire form.  The `schema_version`
// field of `payload` is overwritten with `kCursorVersion`.
[[nodiscard]] std::string encode(EventCursorPayload payload);

// Deserialize an opaque cursor blob.  Returns `nullopt` if the
// buffer is empty, oversized, malformed XDR, or carries a schema
// version other than `kCursorVersion`, invalid method/order/height, or a
// nonzero event sequence for a transaction method. Query identity is checked
// separately by the caller before coverage checks.
[[nodiscard]] std::optional<EventCursorPayload> decode(std::string_view bytes);

} // namespace hogd::cursor
