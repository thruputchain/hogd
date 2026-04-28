#include "hogd/event/topic_layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "event.hh"
#include "indexer.hh"

namespace hogd {

std::optional<std::span<const std::uint8_t, kTopicSizeBytes>>
topic_view(TxEmittedEvent const &event, std::uint32_t index)
{
  if (index >= event.num_topics) {
    return std::nullopt;
  }
  const std::size_t offset = static_cast<std::size_t>(index) * kTopicSizeBytes;
  if (event.serialized_event.size() < offset + kTopicSizeBytes) {
    // Malformed event: num_topics claims more topics than
    // serialized_event can hold.  Treat as no topic at this
    // position.
    return std::nullopt;
  }
  return std::span<const std::uint8_t, kTopicSizeBytes>(
      reinterpret_cast<const std::uint8_t *>(event.serialized_event.data()) +
          offset,
      kTopicSizeBytes);
}

namespace {

// Topic comparison against a list (OR within position).  Empty
// list is a wildcard match.  An out-of-range topic on the event
// (nullopt) fails the match iff the filter list is non-empty.
bool
topic_matches_list(
    std::span<const Topic> filter_list,
    std::optional<std::span<const std::uint8_t, kTopicSizeBytes>> event_topic)
{
  if (filter_list.empty()) {
    return true;
  }
  if (!event_topic) {
    return false;
  }
  for (Topic const &t : filter_list) {
    if (std::ranges::equal(t, *event_topic)) {
      return true;
    }
  }
  return false;
}

} // namespace

bool
event_topics_match(EventTopicFilter const &filter, TxEmittedEvent const &event)
{
  // x23::Vec<Topic, N> models std::ranges::contiguous_range, so it
  // converts implicitly to std::span<const Topic>.
  return topic_matches_list(filter.topic0, topic_view(event, 0)) &&
         topic_matches_list(filter.topic1, topic_view(event, 1)) &&
         topic_matches_list(filter.topic2, topic_view(event, 2));
}

} // namespace hogd
