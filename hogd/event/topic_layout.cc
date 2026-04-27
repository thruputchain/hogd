#include "hogd/event/topic_layout.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
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
  const std::size_t offset =
      static_cast<std::size_t>(index) * kTopicSizeBytes;
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
    if (std::memcmp(t.data(), event_topic->data(), kTopicSizeBytes) == 0) {
      return true;
    }
  }
  return false;
}

} // namespace

bool
event_topics_match(EventTopicFilter const &filter, TxEmittedEvent const &event)
{
  // x23::Vec<Topic, ...> exposes data()/size(); construct an
  // explicit std::span to keep the matcher's interface concrete
  // (no templates over container type).
  return topic_matches_list(std::span<const Topic>(filter.topic0.data(),
                                                   filter.topic0.size()),
                            topic_view(event, 0)) &&
         topic_matches_list(std::span<const Topic>(filter.topic1.data(),
                                                   filter.topic1.size()),
                            topic_view(event, 1)) &&
         topic_matches_list(std::span<const Topic>(filter.topic2.data(),
                                                   filter.topic2.size()),
                            topic_view(event, 2));
}

} // namespace hogd
