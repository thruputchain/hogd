// -*-C++-*-

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>

#include "event.hh"
#include "indexer.hh"

namespace hogd {

// Size of a single topic in bytes; defined by the wire format
// (`typedef opaque Topic[32]` in protocol/event.x).
inline constexpr std::size_t kTopicSizeBytes = std::tuple_size_v<Topic>;

// Return a view over topic [index] of [event] (0-indexed).  Returns
// nullopt if [index] is past the event's [num_topics], or if the
// event's `serialized_event` buffer is shorter than the topic
// region implies (malformed event — defensive).
//
// Per protocol/event.x: `serialized_event` is laid out as
// `num_topics * kTopicSizeBytes` bytes of topic data followed by
// the event payload.  `num_topics` is bounded by kMaxTopics.
[[nodiscard]] std::optional<std::span<const std::uint8_t, kTopicSizeBytes>>
topic_view(TxEmittedEvent const &event, std::uint32_t index);

// Positional topic-filter match against the topics encoded in an
// emitted event.
//
// For each topic position (0..kMaxTopics-1):
//   - empty list -> match anything at that position (wildcard);
//   - non-empty list -> match iff the event's topic at that
//     position is in the list.
//
// AND across positions, OR within each position.  Partial match: a
// filter specifying only topic0 matches events with MORE topics
// than the filter mentions.  If the event has FEWER topics than
// the filter specifies a non-empty list at, the event fails the
// match.
[[nodiscard]] bool
event_topics_match(EventTopicFilter const &filter, TxEmittedEvent const &event);

} // namespace hogd
