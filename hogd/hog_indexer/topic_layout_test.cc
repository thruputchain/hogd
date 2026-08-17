#include "hogd/hog_indexer/topic_layout.h"

#include <algorithm>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "event.hh"
#include "indexer.hh"

namespace hogd {

namespace {

// Build a TxEmittedEvent with [num_topics] topics each filled with
// the byte (topic_fill_start + topic_index), plus 4 bytes of payload.
// Same shape as make_event in indexer/db_test.cc.
TxEmittedEvent
make_event(std::uint32_t num_topics, std::uint8_t topic_fill_start)
{
  TxEmittedEvent e;
  e.num_topics = num_topics;
  e.serialized_event.resize((num_topics * kTopicSizeBytes) + 4);
  for (std::uint32_t t = 0; t < num_topics; ++t) {
    for (std::size_t i = 0; i < kTopicSizeBytes; ++i) {
      e.serialized_event[(t * kTopicSizeBytes) + i] =
          static_cast<std::uint8_t>(topic_fill_start + t);
    }
  }
  // Payload bytes
  for (std::size_t i = 0; i < 4; ++i) {
    e.serialized_event[(num_topics * kTopicSizeBytes) + i] = 0xAB;
  }
  return e;
}

Topic
topic_filled(std::uint8_t fill)
{
  Topic t;
  std::ranges::fill(t, fill);
  return t;
}

} // namespace

TEST(TopicLayoutTest, TopicViewZeroTopicsReturnsNullopt)
{
  auto e = make_event(0, 0x00);
  EXPECT_FALSE(topic_view(e, 0).has_value());
  EXPECT_FALSE(topic_view(e, 1).has_value());
  EXPECT_FALSE(topic_view(e, 2).has_value());
}

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST(TopicLayoutTest, TopicViewMaxTopicsReturnsCorrectSlices)
{
  auto e = make_event(/*num_topics=*/3, /*topic_fill_start=*/0x10);
  auto t0 = topic_view(e, 0);
  ASSERT_TRUE(t0.has_value());
  EXPECT_EQ((*t0)[0], 0x10);
  EXPECT_EQ((*t0)[kTopicSizeBytes - 1], 0x10);

  auto t1 = topic_view(e, 1);
  ASSERT_TRUE(t1.has_value());
  EXPECT_EQ((*t1)[0], 0x11);

  auto t2 = topic_view(e, 2);
  ASSERT_TRUE(t2.has_value());
  EXPECT_EQ((*t2)[0], 0x12);
}
// NOLINTEND(bugprone-unchecked-optional-access)

TEST(TopicLayoutTest, TopicViewIndexPastNumTopicsReturnsNullopt)
{
  auto e = make_event(/*num_topics=*/1, /*topic_fill_start=*/0x10);
  EXPECT_TRUE(topic_view(e, 0).has_value());
  EXPECT_FALSE(topic_view(e, 1).has_value());
  EXPECT_FALSE(topic_view(e, 2).has_value());
}

TEST(TopicLayoutTest, TopicViewMalformedEventReturnsNullopt)
{
  // num_topics claims 3 but serialized_event is only one topic
  // worth of bytes — defensive guard should treat as missing.
  TxEmittedEvent e;
  e.num_topics = 3;
  e.serialized_event.resize(kTopicSizeBytes); // only one topic of bytes
  EXPECT_TRUE(topic_view(e, 0).has_value());
  EXPECT_FALSE(topic_view(e, 1).has_value());
  EXPECT_FALSE(topic_view(e, 2).has_value());
}

TEST(TopicLayoutTest, EventTopicsMatchEmptyFilterMatchesAnyEvent)
{
  EventTopicFilter filter; // all topic lists empty
  auto e = make_event(/*num_topics=*/2, /*topic_fill_start=*/0x42);
  EXPECT_TRUE(event_topics_match(filter, e));

  auto empty_event = make_event(/*num_topics=*/0, /*topic_fill_start=*/0);
  EXPECT_TRUE(event_topics_match(filter, empty_event));
}

TEST(TopicLayoutTest, EventTopicsMatchPositionalAndAcrossOrWithin)
{
  // Filter: topic0 in {0x10}, topic1 in {0x42, 0x43}, topic2 wildcard.
  EventTopicFilter filter;
  filter.topic0.push_back(topic_filled(0x10));
  filter.topic1.push_back(topic_filled(0x42));
  filter.topic1.push_back(topic_filled(0x43));

  // Match: topic0=0x10, topic1=0x42, topic2=anything (3 topics).
  auto match = make_event(3, 0x10);                // fill 0x10, 0x11, 0x12
  EXPECT_FALSE(event_topics_match(filter, match)); // topic1=0x11, not 0x42/43

  // Build an event with explicitly-controlled topics.
  TxEmittedEvent e;
  e.num_topics = 2;
  e.serialized_event.resize(2 * kTopicSizeBytes);
  for (std::size_t i = 0; i < kTopicSizeBytes; ++i) {
    e.serialized_event[i] = 0x10;
  }
  for (std::size_t i = 0; i < kTopicSizeBytes; ++i) {
    e.serialized_event[kTopicSizeBytes + i] = 0x42;
  }
  EXPECT_TRUE(event_topics_match(filter, e));
}

TEST(TopicLayoutTest, EventTopicsMatchPartialMatchExtraTopicsIgnored)
{
  // Filter specifies only topic0 = {0x10}.  An event with topic0=0x10
  // and additional topics still matches.
  EventTopicFilter filter;
  filter.topic0.push_back(topic_filled(0x10));

  TxEmittedEvent e;
  e.num_topics = 3;
  e.serialized_event.resize(3 * kTopicSizeBytes);
  for (std::size_t i = 0; i < kTopicSizeBytes; ++i) {
    e.serialized_event[i] = 0x10;
    e.serialized_event[kTopicSizeBytes + i] = 0xAA;
    e.serialized_event[(2 * kTopicSizeBytes) + i] = 0xBB;
  }
  EXPECT_TRUE(event_topics_match(filter, e));
}

TEST(TopicLayoutTest, EventTopicsMatchEventTooShortFails)
{
  // Filter requires non-empty list at topic1, event has only 1 topic.
  EventTopicFilter filter;
  filter.topic1.push_back(topic_filled(0x10));

  auto e = make_event(/*num_topics=*/1, /*topic_fill_start=*/0x10);
  EXPECT_FALSE(event_topics_match(filter, e));
}

} // namespace hogd
