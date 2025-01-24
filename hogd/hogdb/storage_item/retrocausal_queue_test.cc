#include <gtest/gtest.h>

#include "hogd/hogdb/storage_item/retrocausal_queue.h"

namespace hogd {

namespace {
QueueEntry
make_test_queue_entry(uint8_t value)
{
  QueueEntry entry;
  entry.fill(0);
  entry[0] = value;
  return entry;
}
} // namespace

TEST(RetrocausalQueue, InsertThenConsume)
{

  RetrocausalQueue queue(1);

  queue.apply_insert_delta(
      {{make_test_queue_entry(1), make_test_queue_entry(5)}});
  queue.apply_insert_delta(
      {{make_test_queue_entry(2), make_test_queue_entry(6)}});
  // will be sorted once canonicalized.

  auto entries = queue.get_current_block_entries();
  EXPECT_EQ(entries.size(), 4);

  queue.apply_consume_delta(
      {.past_block_consume = {0, 0},
       .current_block_consumed = {make_test_queue_entry(6)}});

  queue.canonicalize_and_commit_current_block();

  RetrocausalQueue next_queue(queue, 2);

  const auto *next_entries = next_queue.try_get_next_block(0);
  ASSERT_TRUE(next_entries);
  EXPECT_EQ(next_entries->entries.size(), 3);
  EXPECT_EQ(next_entries->entries[0], make_test_queue_entry(1));
  EXPECT_EQ(next_entries->entries[1], make_test_queue_entry(2));
  EXPECT_EQ(next_entries->entries[2], make_test_queue_entry(5));

  const auto *next_entries2 = next_queue.try_get_next_block(1);
  // assert that queries starting from 0 and 1 return the same result
  EXPECT_EQ(next_entries, next_entries2);
}

} // namespace hogd
