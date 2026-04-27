#include "hogd/indexer/event_cursor.h"

#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "event.hh"
#include "hogd/base/types.h"
#include "indexer.hh"
#include "pagination.hh"
#include "types.hh"

namespace hogd {

namespace {

void
write_be64(std::span<uint8_t, 8> out, uint64_t v)
{
  for (int i = 7; i >= 0; --i) {
    out[i] = static_cast<uint8_t>(v & 0xFF);
    v >>= 8;
  }
}

uint64_t
read_be64(std::span<const uint8_t, 8> in)
{
  uint64_t v = 0;
  for (size_t i = 0; i < 8; ++i) {
    v = (v << 8) | in[i];
  }
  return v;
}

void
write_be32(std::span<uint8_t, 4> out, uint32_t v)
{
  for (int i = 3; i >= 0; --i) {
    out[i] = static_cast<uint8_t>(v & 0xFF);
    v >>= 8;
  }
}

uint32_t
read_be32(std::span<const uint8_t, 4> in)
{
  uint32_t v = 0;
  for (size_t i = 0; i < 4; ++i) {
    v = (v << 8) | in[i];
  }
  return v;
}

} // namespace

std::string
EventCursor::encode(IterationOrder order, BlockHeight block_height,
                    SHA256Hash const &tx_hash, uint32_t sequence_number)
{
  std::string out(kEncodedSize, '\0');
  auto *data = reinterpret_cast<uint8_t *>(out.data());
  data[0] = kVersion;
  data[1] = static_cast<uint8_t>(order);
  // BlockHeight is a signed int64; cast through uint64_t preserves
  // bit pattern so negative values round-trip.
  write_be64(std::span<uint8_t, 8>(data + 2, 8),
             static_cast<uint64_t>(block_height));
  std::memcpy(data + 10, tx_hash.data(), 32);
  write_be32(std::span<uint8_t, 4>(data + 42, 4), sequence_number);
  return out;
}

std::optional<EventCursor::Decoded>
EventCursor::decode(std::string_view cursor)
{
  if (cursor.size() != kEncodedSize) {
    return std::nullopt;
  }
  const auto *data = reinterpret_cast<const uint8_t *>(cursor.data());
  if (data[0] != kVersion) {
    return std::nullopt;
  }
  const uint8_t order_byte = data[1];
  if (order_byte !=
          static_cast<uint8_t>(IterationOrder::IterationOrderAscending) &&
      order_byte !=
          static_cast<uint8_t>(IterationOrder::IterationOrderDescending)) {
    return std::nullopt;
  }

  Decoded d;
  d.order = static_cast<IterationOrder>(order_byte);
  d.block_height = static_cast<BlockHeight>(
      read_be64(std::span<const uint8_t, 8>(data + 2, 8)));
  std::memcpy(d.tx_hash.data(), data + 10, 32);
  d.sequence_number = read_be32(std::span<const uint8_t, 4>(data + 42, 4));
  return d;
}

namespace {

// Extract topic N (0-based) from an emitted event's serialized_event
// prefix.  Returns a span over the 32-byte topic value if present,
// or nullopt if the event has fewer than N+1 topics.
//
// Per protocol/event.x: serialized_event is laid out as
// `num_topics * 32` bytes of topic data followed by the event
// payload.  `num_topics` is bounded by kMaxTopics == 3.
std::optional<std::span<const uint8_t, 32>>
topic_at(TxEmittedEvent const &event, uint32_t index)
{
  if (index >= event.num_topics) {
    return std::nullopt;
  }
  const size_t offset = static_cast<size_t>(index) * 32;
  if (event.serialized_event.size() < offset + 32) {
    // Malformed event: num_topics claims more topics than
    // serialized_event can hold.  Treat as no topic at this position.
    return std::nullopt;
  }
  return std::span<const uint8_t, 32>(
      reinterpret_cast<const uint8_t *>(event.serialized_event.data()) + offset,
      32);
}

template<typename VecT>
bool
topic_matches_list(VecT const &filter_list,
                   std::optional<std::span<const uint8_t, 32>> event_topic)
{
  if (filter_list.empty()) {
    // Wildcard — any topic (including "event has no topic here")
    // matches.
    return true;
  }
  if (!event_topic) {
    // Filter specifies a non-empty list at this position, but the
    // event has no topic here.  Fails the match.
    return false;
  }
  for (Topic const &t : filter_list) {
    if (std::memcmp(t.data(), event_topic->data(), 32) == 0) {
      return true;
    }
  }
  return false;
}

} // namespace

bool
event_topics_match(EventTopicFilter const &filter, TxEmittedEvent const &event)
{
  return topic_matches_list(filter.topic0, topic_at(event, 0)) &&
         topic_matches_list(filter.topic1, topic_at(event, 1)) &&
         topic_matches_list(filter.topic2, topic_at(event, 2));
}

} // namespace hogd
