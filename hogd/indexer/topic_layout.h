#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>

#include "event.hh"
#include "indexer.hh"

namespace hogd {

inline constexpr std::size_t kTopicSizeBytes = std::tuple_size_v<Topic>;

[[nodiscard]] std::optional<std::span<const std::uint8_t, kTopicSizeBytes>>
topic_view(TxEmittedEvent const &event, std::uint32_t index);

[[nodiscard]] bool event_topics_match(EventTopicFilter const &filter,
                                      TxEmittedEvent const &event);

} // namespace hogd
