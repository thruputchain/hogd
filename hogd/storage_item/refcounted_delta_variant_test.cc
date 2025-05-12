#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/storage_item/invalid_state_flag.h"
#include "hogd/storage_item/refcounted_delta_variant.h"
#include "hogd/storage_item/retrocausal_queue.h"
#include "hogd/storage_item/storage_costs.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"

namespace hogd {
namespace {
QueueEntry
make_test_queue_entry(uint8_t value)
{
  return {value};
}
} // namespace

TEST(RefcountedDeltaVariant, RCQConsumeValidity)
{
  RefcountedDeltaVariant variant;
  InvalidStateFlag invalid_state_flag;
  auto *arg = variant.try_set_variant<RetrocausalQueueArgs>();
  ASSERT_TRUE(arg);

  ASSERT_TRUE(arg->try_reserve_consume());
  ASSERT_FALSE(arg->try_reserve_consume());

  arg->revert_consume();
  ASSERT_TRUE(arg->try_reserve_consume());

  arg->commit_consume(
      RetrocausalQueueConsumeDelta{
          .current_block_consumed = {make_test_queue_entry(1),
                                     make_test_queue_entry(2)}},
      invalid_state_flag);

  ASSERT_TRUE(invalid_state_flag.is_invalid());

  arg->commit_insert(
      RetrocausalQueueInsertDelta{.added_entries = {make_test_queue_entry(1),
                                                    make_test_queue_entry(3)}},
      invalid_state_flag);

  ASSERT_TRUE(invalid_state_flag.is_invalid());

  arg->commit_insert(
      RetrocausalQueueInsertDelta{.added_entries = {make_test_queue_entry(2),
                                                    make_test_queue_entry(4)}},
      invalid_state_flag);

  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

TEST(RefcountedDeltaVariant, RCQDuplicateInsert)
{
  RefcountedDeltaVariant variant;
  InvalidStateFlag invalid_state_flag;
  auto *arg = variant.try_set_variant<RetrocausalQueueArgs>();
  ASSERT_TRUE(arg);
  ASSERT_TRUE(arg->try_reserve_consume());

  arg->commit_consume(
      RetrocausalQueueConsumeDelta{
          .current_block_consumed = {make_test_queue_entry(1),
                                     make_test_queue_entry(1)}},
      invalid_state_flag);

  ASSERT_TRUE(invalid_state_flag.is_invalid());

  arg->commit_insert(
      RetrocausalQueueInsertDelta{.added_entries = {make_test_queue_entry(1),
                                                    make_test_queue_entry(2)}},
      invalid_state_flag);

  ASSERT_TRUE(invalid_state_flag.is_invalid());

  arg->commit_insert(
      RetrocausalQueueInsertDelta{.added_entries = {make_test_queue_entry(1),
                                                    make_test_queue_entry(3)}},
      invalid_state_flag);

  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

namespace {
auto
make_rcq_delta(std::optional<RetrocausalQueueConsumeDelta> consume_delta,
               RetrocausalQueueInsertDelta insert_delta)
{
  StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
  if (consume_delta) {
    delta.retrocausal_queue().consume.emplace(*consume_delta);
  }
  delta.retrocausal_queue().insert = std::move(insert_delta);
  return delta;
}
} // namespace

TEST(RefcountedDeltaVariant, RCQCosts)
{
  RefcountedDeltaVariant variant;
  InvalidStateFlag invalid_state_flag;
  auto *arg = variant.try_set_variant<RetrocausalQueueArgs>();
  ASSERT_TRUE(arg);

  RetrocausalQueue queue(0);
  RetrocausalQueue queue2(
      queue,
      RetrocausalQueuePastBlockConsumeDelta{.max_fully_consumed_block_number =
                                                0,
                                            .partial_consumed_number = 0},
      RetrocausalQueueBlock{.entries = {make_test_queue_entry(1),
                                        make_test_queue_entry(2)},
                            .block_number = 1});

  auto item = std::make_shared<const StorageItemWrapper>(
      StorageItemWrapper::RetrocausalQueueMarker{}, std::move(queue2));

  EXPECT_EQ(
      item->storage_cost(),
      variant.get_cost_after_commit(make_rcq_delta(std::nullopt,
                                                   RetrocausalQueueInsertDelta{
                                                       .added_entries = {}}),
                                    item.get()));

  EXPECT_EQ(
      item->storage_cost() + (2 * kRCQCostPerEntry) + kRCQCostPerBlock,
      variant.get_cost_after_commit(
          make_rcq_delta(std::nullopt,
                         RetrocausalQueueInsertDelta{
                             .added_entries = {make_test_queue_entry(1),
                                               make_test_queue_entry(2)}}),
          item.get()));

  ASSERT_TRUE(arg->try_reserve_consume());
  EXPECT_EQ(item->storage_cost() - (2 * kRCQCostPerEntry),
            variant.get_cost_after_commit(
                make_rcq_delta(
                    RetrocausalQueueConsumeDelta{
                        .past_block_consume =
                            RetrocausalQueuePastBlockConsumeDelta{
                                .max_fully_consumed_block_number = 0,
                                .partial_consumed_number = 1},
                        .current_block_consumed = {make_test_queue_entry(1)}},
                    RetrocausalQueueInsertDelta{.added_entries = {}}),
                item.get()));
  // same test case as above, but after a try_reserve_consume has been called.
  EXPECT_EQ(
      item->storage_cost() + (2 * kRCQCostPerEntry) + kRCQCostPerBlock,
      variant.get_cost_after_commit(
          make_rcq_delta(std::nullopt,
                         RetrocausalQueueInsertDelta{
                             .added_entries = {make_test_queue_entry(1),
                                               make_test_queue_entry(2)}}),
          item.get()));

  arg->commit_consume(
      RetrocausalQueueConsumeDelta{
          .past_block_consume =
              RetrocausalQueuePastBlockConsumeDelta{
                  .max_fully_consumed_block_number = 0,
                  .partial_consumed_number = 1},
          .current_block_consumed = {make_test_queue_entry(1)}},
      invalid_state_flag);

  EXPECT_EQ(
      item->storage_cost() - (2 * kRCQCostPerEntry),
      variant.get_cost_after_commit(make_rcq_delta(std::nullopt,
                                                   RetrocausalQueueInsertDelta{
                                                       .added_entries = {}}),
                                    item.get()));
  // insertion, even those that are consumed, still create a new block
  EXPECT_EQ(
      item->storage_cost() - kRCQCostPerEntry + kRCQCostPerBlock,
      variant.get_cost_after_commit(
          make_rcq_delta(std::nullopt,
                         RetrocausalQueueInsertDelta{
                             .added_entries = {make_test_queue_entry(1)}}),
          item.get()));
  arg->commit_insert(
      RetrocausalQueueInsertDelta{.added_entries = {make_test_queue_entry(1)}},
      invalid_state_flag);
  ASSERT_FALSE(invalid_state_flag.is_invalid());

  EXPECT_EQ(
      item->storage_cost() + kRCQCostPerBlock - kRCQCostPerEntry,
      variant.get_cost_after_commit(make_rcq_delta(std::nullopt,
                                                   RetrocausalQueueInsertDelta{
                                                       .added_entries = {}}),
                                    item.get()));
}

} // namespace hogd
