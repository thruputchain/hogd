#include "hogd/hog_indexer/topic_layout.h"

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
    return std::nullopt;
  }
  return std::span<const std::uint8_t, kTopicSizeBytes>(
      reinterpret_cast<const std::uint8_t *>(event.serialized_event.data()) +
          offset,
      kTopicSizeBytes);
}

namespace {

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
  return std::ranges::any_of(filter_list, [&](Topic const &topic) {
    return std::ranges::equal(topic, *event_topic);
  });
}

} // namespace

bool
event_topics_match(EventTopicFilter const &filter, TxEmittedEvent const &event)
{
  return topic_matches_list(filter.topic0, topic_view(event, 0)) &&
         topic_matches_list(filter.topic1, topic_view(event, 1)) &&
         topic_matches_list(filter.topic2, topic_view(event, 2));
}

} // namespace hogd
