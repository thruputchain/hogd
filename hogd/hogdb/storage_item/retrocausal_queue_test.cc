#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/hogdb/storage_item/retrocausal_queue.h"
#include "storage_item.hh"

namespace hogd {

namespace {
QueueEntry
make_test_queue_entry(uint8_t value)
{
  return {value};
}

} // namespace

TEST(RetrocausalQueue, InsertThenConsume)
{
  RetrocausalQueue queue(0);

  RetrocausalQueue queue2(
      queue,
      {.max_fully_consumed_block_number = 0, .partial_consumed_number = 0},
      {.entries = {make_test_queue_entry(1), make_test_queue_entry(2),
                   make_test_queue_entry(5), make_test_queue_entry(6)},
       .block_number = 1});

  auto const *blk = queue2.try_get_next_block(1);
  ASSERT_TRUE(blk);
  EXPECT_EQ(blk->block_number, 1);
  EXPECT_EQ(blk->entries.size(), 4);

  RetrocausalQueue queue3(queue2,
                          {.max_fully_consumed_block_number = 0,
                           .partial_consumed_number = 3},
                          {.entries = {}, .block_number = 2});

  auto const *blk2 = queue3.try_get_next_block(1);
  ASSERT_TRUE(blk2);
  EXPECT_EQ(blk2->block_number, 1);
  EXPECT_EQ(blk2->entries.size(), 1);
}

TEST(RetrocausalQueue, ClearSeveralBlocks)
{
  std::unique_ptr<RetrocausalQueue> queue =
      std::make_unique<RetrocausalQueue>(0);

  for (uint64_t i = 0; i < 50; i++) {

    queue = std::make_unique<RetrocausalQueue>(
        *queue,
        RetrocausalQueuePastBlockConsumeDelta{.max_fully_consumed_block_number =
                                                  0,
                                              .partial_consumed_number = 0},
        RetrocausalQueueBlock{.entries = {make_test_queue_entry(i),
                                          make_test_queue_entry(i + 50)},
                              .block_number = i + 1});
  }

  queue = std::make_unique<RetrocausalQueue>(
      *queue,
      RetrocausalQueuePastBlockConsumeDelta{.max_fully_consumed_block_number =
                                                25,
                                            .partial_consumed_number = 1},
      RetrocausalQueueBlock{.entries = {}, .block_number = 52});

  const auto *next_entries = queue->try_get_next_block(0);
  ASSERT_TRUE(next_entries);
  EXPECT_EQ(next_entries->entries.size(), 1);
  EXPECT_EQ(next_entries->block_number, 26);
  EXPECT_EQ(next_entries->entries[0], make_test_queue_entry(75));
}

} // namespace hogd
