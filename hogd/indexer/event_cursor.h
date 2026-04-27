// -*-C++-*-

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "event.hh"
#include "indexer.hh"
#include "pagination.hh"
#include "types.hh"

namespace hogd {

// Opaque, versioned pagination cursor used by get_events.
//
// Encoding (46 bytes):
//   byte[0]      version tag (currently kVersion == 0x01)
//   byte[1]      IterationOrder (0=Asc, 1=Desc)
//   byte[2..10)  block_height of last emitted event (8 bytes, big-endian)
//   byte[10..42) tx_hash of last emitted event (32 bytes)
//   byte[42..46) sequence_number of last emitted event (4 bytes, BE)
//
// The cursor encodes the order so the caller can't flip direction
// mid-stream; on decode we validate it matches what the caller asked
// for (or, if the caller supplied a cursor, we use the cursor's order
// and ignore the request's `order` field).
//
// Exclusive: the stored position is the LAST event emitted in the
// previous response; the next response resumes strictly past it.
class EventCursor {
public:
  static constexpr uint8_t kVersion = 0x01;
  static constexpr size_t kEncodedSize = 1 + 1 + 8 + 32 + 4;

  struct Decoded {
    IterationOrder order;
    BlockHeight block_height;
    SHA256Hash tx_hash;
    uint32_t sequence_number;
  };

  // Produce the encoded cursor bytes for the given "last emitted"
  // position.
  static std::string encode(IterationOrder order, BlockHeight block_height,
                            SHA256Hash const &tx_hash,
                            uint32_t sequence_number);

  // Decode.  Returns nullopt if the cursor has an unknown version,
  // the wrong length, or an out-of-range order byte.  Empty input
  // is "no cursor" and also returns nullopt (caller must branch on
  // cursor.empty() before decoding).
  static std::optional<Decoded> decode(std::string_view cursor);
};

// Limit clamping constants for get_events.  Limit of 0 is interpreted
// as "server default"; any value outside [1, kMax] is clamped.
constexpr uint32_t kEventsDefaultLimit = 50;
constexpr uint32_t kEventsMaxLimit = 200;

inline uint32_t
clamp_events_limit(uint32_t requested)
{
  if (requested == 0) {
    return kEventsDefaultLimit;
  }
  if (requested > kEventsMaxLimit) {
    return kEventsMaxLimit;
  }
  return requested;
}

// Positional topic-filter match against the topics encoded in an
// emitted event.
//
// Each event's serialized_event starts with `num_topics * 32` bytes
// of topic data (per protocol/event.x), followed by the event payload.
// The filter specifies, per position (0, 1, 2):
//   - empty list -> match anything at that position (wildcard);
//   - non-empty list -> match iff the event's topic at that position
//     is in the list.
//
// AND across positions, OR within each position.  Partial match: a
// filter specifying only topic0 matches events with MORE topics than
// the filter mentions.  If the event has FEWER topics than the filter
// specifies a non-empty list at, the event fails the match.
bool event_topics_match(EventTopicFilter const &filter,
                        TxEmittedEvent const &event);

} // namespace hogd
