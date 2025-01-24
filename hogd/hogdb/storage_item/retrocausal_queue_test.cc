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
      {.past_block_consume = {.max_fully_consumed_block_number=0, .partial_consumed_number=0},
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

  // consume some of the entries in this block
  next_queue.apply_consume_delta(
      {.past_block_consume = {.max_fully_consumed_block_number=0, .partial_consumed_number=2},
       .current_block_consumed = {}});

  next_queue.canonicalize_and_commit_current_block();

  RetrocausalQueue next_queue2(next_queue, 3);

  const auto *next_entries3 = next_queue2.try_get_next_block(0);
  ASSERT_TRUE(next_entries3);
  EXPECT_EQ(next_entries3->entries.size(), 1);
  EXPECT_EQ(next_entries3->entries[0], make_test_queue_entry(5));
}

TEST(RetrocausalQueue, ClearSeveralBlocks)
{
    std::unique_ptr<RetrocausalQueue> queue = std::make_unique<RetrocausalQueue>(1);

    for (int i = 0; i < 50; i++) {
        queue->apply_insert_delta({{make_test_queue_entry(i), make_test_queue_entry(i+50)}});
        queue->canonicalize_and_commit_current_block();

        queue = std::make_unique<RetrocausalQueue>(*queue, i+1);
    }

    queue->apply_consume_delta({.past_block_consume = {.max_fully_consumed_block_number=25, .partial_consumed_number=1},
                                .current_block_consumed = {}});
    queue->canonicalize_and_commit_current_block();

    queue = std::make_unique<RetrocausalQueue>(*queue, 51);

    const auto *next_entries = queue->try_get_next_block(0);
    ASSERT_TRUE(next_entries);
    EXPECT_EQ(next_entries->entries.size(), 1);
    EXPECT_EQ(next_entries->block_number, 26);
    EXPECT_EQ(next_entries->entries[0], make_test_queue_entry(76));
}

} // namespace hogd
