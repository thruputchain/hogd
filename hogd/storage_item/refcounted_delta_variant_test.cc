#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/invalid_state_flag.h"
#include "hogd/storage_item/refcounted_delta_variant.h"
#include "hogd/storage_item/retrocausal_queue.h"
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

const StorageCostModel cost_model(12000, 1, 5, 100);

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
  // Initial queue state: 1 block, 2 entries (1, 2)

  auto item = std::make_shared<const StorageItemWrapper>(std::move(queue2),
                                                         &cost_model);
  EXPECT_EQ(item->storage_cost(),
            cost_model.kBaseItemCost +
                cost_model.get_retrocausal_queue_cost(2, 1));

  EXPECT_EQ(0, variant.get_marginal_cost_upper_bound(
                   make_rcq_delta(std::nullopt,
                                  RetrocausalQueueInsertDelta{
                                      .added_entries = {}}),
                   &cost_model, item.get()));

  EXPECT_EQ(
      (2 * cost_model.kRCQCostPerEntry) + cost_model.kRCQCostPerBlock,
      variant.get_marginal_cost_upper_bound(
          make_rcq_delta(std::nullopt,
                         RetrocausalQueueInsertDelta{
                             .added_entries = {make_test_queue_entry(1),
                                               make_test_queue_entry(2)}}),
          &cost_model, item.get()));

  ASSERT_TRUE(arg->try_reserve_consume());
  EXPECT_EQ(0,
            variant.get_marginal_cost_upper_bound(
                make_rcq_delta(
                    RetrocausalQueueConsumeDelta{
                        .past_block_consume =
                            RetrocausalQueuePastBlockConsumeDelta{
                                .max_fully_consumed_block_number = 0,
                                .partial_consumed_number = 1},
                        .current_block_consumed = {make_test_queue_entry(1)}},
                    RetrocausalQueueInsertDelta{.added_entries = {}}),
                &cost_model, item.get()));
  // same test case as above, but after a try_reserve_consume has been called.
  EXPECT_EQ(
      (2 * cost_model.kRCQCostPerEntry) + cost_model.kRCQCostPerBlock,
      variant.get_marginal_cost_upper_bound(
          make_rcq_delta(std::nullopt,
                         RetrocausalQueueInsertDelta{
                             .added_entries = {make_test_queue_entry(1),
                                               make_test_queue_entry(2)}}),
          &cost_model, item.get()));

  // consume 1 entry from the first block and 1 entry from the current block
  // (which hasn't been inserted yet)
  arg->commit_consume(
      RetrocausalQueueConsumeDelta{
          .past_block_consume =
              RetrocausalQueuePastBlockConsumeDelta{
                  .max_fully_consumed_block_number = 0,
                  .partial_consumed_number = 1},
          .current_block_consumed = {make_test_queue_entry(1)}},
      invalid_state_flag);
  EXPECT_TRUE(invalid_state_flag.is_invalid());

  // Past block consume doesn't create a new block.
  EXPECT_EQ(0, variant.get_marginal_cost_upper_bound(
                   make_rcq_delta(std::nullopt,
                                  RetrocausalQueueInsertDelta{
                                      .added_entries = {}}),
                   &cost_model, item.get()));

  {
    auto item2 = variant.to_item(item, &cost_model);
    EXPECT_EQ(item2->storage_cost(),
              cost_model.kBaseItemCost +
                  cost_model.get_retrocausal_queue_cost(1, 1));
    EXPECT_TRUE(invalid_state_flag.is_invalid());
  }

  // During the marginal cost upper bound, we don't check to see whether the
  // entries being committed were already consumed.
  EXPECT_EQ(
      cost_model.kRCQCostPerEntry + cost_model.kRCQCostPerBlock,
      variant.get_marginal_cost_upper_bound(
          make_rcq_delta(std::nullopt,
                         RetrocausalQueueInsertDelta{
                             .added_entries = {make_test_queue_entry(1)}}),
          &cost_model, item.get()));
  arg->commit_insert(
      RetrocausalQueueInsertDelta{.added_entries = {make_test_queue_entry(1)}},
      invalid_state_flag);
  ASSERT_FALSE(invalid_state_flag.is_invalid());

  // After previous state, there still isn't a new block, so this insert would
  // create a new one.
  EXPECT_EQ(
      cost_model.kRCQCostPerEntry + cost_model.kRCQCostPerBlock,
      variant.get_marginal_cost_upper_bound(
          make_rcq_delta(std::nullopt,
                         RetrocausalQueueInsertDelta{
                             .added_entries = {make_test_queue_entry(2)}}),
          &cost_model, item.get()));
  auto item2 = variant.to_item(item, &cost_model);
  auto stats = item2->queue().get_active_stats();
  EXPECT_EQ(stats.active_block_count, 1);
  EXPECT_EQ(stats.active_size, 1);

  EXPECT_EQ(item2->storage_cost(),
            cost_model.kBaseItemCost +
                cost_model.get_retrocausal_queue_cost(1, 1));
  EXPECT_FALSE(invalid_state_flag.is_invalid());
}

TEST(RefcountedDeltaVariant, RCQCostsBlockCountUpperBound)
{
  RefcountedDeltaVariant variant;
  InvalidStateFlag invalid_state_flag;
  auto *arg = variant.try_set_variant<RetrocausalQueueArgs>();
  ASSERT_TRUE(arg);

  // set up an initial queue with 1 block, 5 entries
  RetrocausalQueue queue(0);
  RetrocausalQueue queue2(
      queue,
      RetrocausalQueuePastBlockConsumeDelta{.max_fully_consumed_block_number =
                                                0,
                                            .partial_consumed_number = 0},
      RetrocausalQueueBlock{.entries = {make_test_queue_entry(1),
                                        make_test_queue_entry(2),
                                        make_test_queue_entry(3),
                                        make_test_queue_entry(4),
                                        make_test_queue_entry(5)},
                            .block_number = 1});

  auto item = std::make_shared<const StorageItemWrapper>(std::move(queue2),
                                                         &cost_model);
  EXPECT_EQ(item->storage_cost(),
            cost_model.kBaseItemCost +
                cost_model.get_retrocausal_queue_cost(5, 1));

  // inserting entries creates a new block
  EXPECT_EQ(
      (2 * cost_model.kRCQCostPerEntry) + cost_model.kRCQCostPerBlock,
      variant.get_marginal_cost_upper_bound(
          make_rcq_delta(std::nullopt,
                         RetrocausalQueueInsertDelta{
                             .added_entries = {make_test_queue_entry(1),
                                               make_test_queue_entry(2)}}),
          &cost_model, item.get()));

  ASSERT_TRUE(arg->try_reserve_consume());

  // check cost accounting for upper bounds for reserving a consume
  EXPECT_EQ(0, variant.get_marginal_cost_upper_bound(
                   make_rcq_delta(
                       RetrocausalQueueConsumeDelta{
                           .past_block_consume =
                               RetrocausalQueuePastBlockConsumeDelta{
                                   .max_fully_consumed_block_number = 0,
                                   .partial_consumed_number = 1},
                           .current_block_consumed = {}},
                       RetrocausalQueueInsertDelta{.added_entries = {}}),
                   &cost_model, item.get()));

  // create 2 entries and consume 2 entries from current block; one from past
  // block This requires paying for the new block, as there's an insertion
  EXPECT_EQ(cost_model.kRCQCostPerBlock,
            variant.get_marginal_cost_upper_bound(
                make_rcq_delta(
                    RetrocausalQueueConsumeDelta{
                        .past_block_consume =
                            RetrocausalQueuePastBlockConsumeDelta{
                                .max_fully_consumed_block_number = 0,
                                .partial_consumed_number = 1},
                        .current_block_consumed = {make_test_queue_entry(1),
                                                   make_test_queue_entry(2)}},
                    RetrocausalQueueInsertDelta{
                        .added_entries = {make_test_queue_entry(1),
                                          make_test_queue_entry(4)}}),
                &cost_model, item.get()));

  // Now we do a past block consume, which doesn't create a new block
  arg->commit_consume(
      RetrocausalQueueConsumeDelta{.past_block_consume =
                                       RetrocausalQueuePastBlockConsumeDelta{
                                           .max_fully_consumed_block_number = 0,
                                           .partial_consumed_number = 1},
                                   .current_block_consumed = {}},
      invalid_state_flag);

  auto item2 = variant.to_item(item, &cost_model);
  auto stats = item2->queue().get_active_stats();
  EXPECT_EQ(stats.active_block_count, 1);
  EXPECT_EQ(stats.active_size, 4);

  // Now we insert an item, which creates a new block
  arg->commit_insert(
      RetrocausalQueueInsertDelta{.added_entries = {make_test_queue_entry(1)}},
      invalid_state_flag);

  auto item3 = variant.to_item(item, &cost_model);
  auto stats2 = item3->queue().get_active_stats();
  EXPECT_EQ(stats2.active_block_count, 2);
  EXPECT_EQ(stats2.active_size, 5);
}

} // namespace hogd
