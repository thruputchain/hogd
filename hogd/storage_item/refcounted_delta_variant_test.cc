#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/invalid_state_flag.h"
#include "hogd/storage_item/refcounted_delta_variant.h"
#include "hogd/storage_item/replay_cache.h"
#include "hogd/storage_item/retrocausal_queue.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "transaction.hh"

namespace hogd {
namespace {
QueueEntry
make_test_queue_entry(uint8_t value)
{
  return {value};
}

const StorageCostModel cost_model(12000, 1, 5, 5, 0);

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
    delta.retrocausal_queue()
        .consume.status(RetrocausalQueueConsume::kRcqConsume)
        .delta() = *consume_delta;
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
  EXPECT_EQ(item->storage_cost(), cost_model.get_retrocausal_queue_cost(2));

  EXPECT_EQ(0, variant.get_marginal_cost_upper_bound(
                   make_rcq_delta(std::nullopt,
                                  RetrocausalQueueInsertDelta{
                                      .added_entries = {}}),
                   &cost_model, item.get()));

  EXPECT_EQ(
      cost_model.get_retrocausal_queue_cost(2),
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
      cost_model.get_retrocausal_queue_cost(2),
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
    EXPECT_EQ(item2->storage_cost(), cost_model.get_retrocausal_queue_cost(1));
    EXPECT_TRUE(invalid_state_flag.is_invalid());
  }

  // During the marginal cost upper bound, we don't check to see whether the
  // entries being committed were already consumed.
  EXPECT_EQ(
      cost_model.get_retrocausal_queue_cost(1),
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
      cost_model.get_retrocausal_queue_cost(1),
      variant.get_marginal_cost_upper_bound(
          make_rcq_delta(std::nullopt,
                         RetrocausalQueueInsertDelta{
                             .added_entries = {make_test_queue_entry(2)}}),
          &cost_model, item.get()));
  auto item2 = variant.to_item(item, &cost_model);
  auto stats = item2->queue().get_active_stats();
  EXPECT_EQ(stats.active_size, 1);

  EXPECT_EQ(item2->storage_cost(), cost_model.get_retrocausal_queue_cost(1));
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
  EXPECT_EQ(item->storage_cost(), cost_model.get_retrocausal_queue_cost(5));

  // inserting entries creates a new block
  EXPECT_EQ(
      cost_model.get_retrocausal_queue_cost(2),
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
  // block. Net inserted entries = 2 - 2 = 0, so only base cost.
  EXPECT_EQ(cost_model.kBaseItemCost,
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
  EXPECT_EQ(stats.active_size, 4);

  // Now we insert an item, which creates a new block
  arg->commit_insert(
      RetrocausalQueueInsertDelta{.added_entries = {make_test_queue_entry(1)}},
      invalid_state_flag);

  auto item3 = variant.to_item(item, &cost_model);
  auto stats2 = item3->queue().get_active_stats();
  EXPECT_EQ(stats2.active_size, 5);
}

TEST(RefcountedDeltaVariant, RCQNoEmptyBlockWhenInsertFullyConsumed)
{
  RefcountedDeltaVariant variant;
  InvalidStateFlag invalid_state_flag;
  auto *arg = variant.try_set_variant<RetrocausalQueueArgs>();
  ASSERT_TRUE(arg);
  ASSERT_TRUE(arg->try_reserve_consume());

  // Consume one current-block entry before its insert, then insert exactly
  // that same entry. This is the validation-mode pairing case that should
  // not create a persisted empty block.
  arg->commit_consume(
      RetrocausalQueueConsumeDelta{
          .past_block_consume =
              RetrocausalQueuePastBlockConsumeDelta{
                  .max_fully_consumed_block_number = 0,
                  .partial_consumed_number = 0},
          .current_block_consumed = {make_test_queue_entry(1)}},
      invalid_state_flag);

  arg->commit_insert(
      RetrocausalQueueInsertDelta{.added_entries = {make_test_queue_entry(1)}},
      invalid_state_flag);
  ASSERT_FALSE(invalid_state_flag.is_invalid());

  auto out = variant.to_item(nullptr, &cost_model);
  ASSERT_TRUE(out);
  ASSERT_EQ(out->type(), StorageItemType::kItemTypeRetrocausalQueue);

  auto const &queue = out->queue();
  EXPECT_EQ(queue.get_active_stats(),
            (RetrocausalQueueStats{.active_size = 0}));
  EXPECT_EQ(queue.get_current_block_number(), 0U);
  EXPECT_EQ(queue.try_get_next_block(1), nullptr);
}

TEST(RefcountedDeltaVariant,
     RCQUpperBoundAddsBlockAfterPastConsumeWhenNoPendingBlock)
{
  RefcountedDeltaVariant variant;
  InvalidStateFlag invalid_state_flag;
  auto *arg = variant.try_set_variant<RetrocausalQueueArgs>();
  ASSERT_TRUE(arg);

  // Base queue: one persisted block with two entries.
  RetrocausalQueue queue(0);
  RetrocausalQueue queue2(
      queue,
      RetrocausalQueuePastBlockConsumeDelta{.max_fully_consumed_block_number =
                                                0,
                                            .partial_consumed_number = 0},
      RetrocausalQueueBlock{.entries = {make_test_queue_entry(1),
                                        make_test_queue_entry(2)},
                            .block_number = 1});
  auto item = std::make_shared<const StorageItemWrapper>(std::move(queue2),
                                                         &cost_model);

  ASSERT_TRUE(arg->try_reserve_consume());
  arg->commit_consume(
      RetrocausalQueueConsumeDelta{.past_block_consume =
                                       RetrocausalQueuePastBlockConsumeDelta{
                                           .max_fully_consumed_block_number = 0,
                                           .partial_consumed_number = 1},
                                   .current_block_consumed = {}},
      invalid_state_flag);
  ASSERT_FALSE(invalid_state_flag.is_invalid());

  // No pending current block yet: first insert must reserve entry cost.
  EXPECT_EQ(
      cost_model.kBaseItemCost + cost_model.kRCQCostPerEntry,
      variant.get_marginal_cost_upper_bound(
          make_rcq_delta(std::nullopt,
                         RetrocausalQueueInsertDelta{
                             .added_entries = {make_test_queue_entry(9)}}),
          &cost_model, item.get()));
}

TEST(RefcountedDeltaVariant,
     RCQUpperBoundSkipsBlockAfterPastConsumeWhenPendingBlockExists)
{
  RefcountedDeltaVariant variant;
  InvalidStateFlag invalid_state_flag;
  auto *arg = variant.try_set_variant<RetrocausalQueueArgs>();
  ASSERT_TRUE(arg);

  // Base queue: one persisted block with two entries.
  RetrocausalQueue queue(0);
  RetrocausalQueue queue2(
      queue,
      RetrocausalQueuePastBlockConsumeDelta{.max_fully_consumed_block_number =
                                                0,
                                            .partial_consumed_number = 0},
      RetrocausalQueueBlock{.entries = {make_test_queue_entry(1),
                                        make_test_queue_entry(2)},
                            .block_number = 1});
  auto item = std::make_shared<const StorageItemWrapper>(std::move(queue2),
                                                         &cost_model);

  ASSERT_TRUE(arg->try_reserve_consume());
  arg->commit_consume(
      RetrocausalQueueConsumeDelta{.past_block_consume =
                                       RetrocausalQueuePastBlockConsumeDelta{
                                           .max_fully_consumed_block_number = 0,
                                           .partial_consumed_number = 1},
                                   .current_block_consumed = {}},
      invalid_state_flag);
  ASSERT_FALSE(invalid_state_flag.is_invalid());

  // Create a pending current block.
  arg->commit_insert(
      RetrocausalQueueInsertDelta{.added_entries = {make_test_queue_entry(8)}},
      invalid_state_flag);
  ASSERT_FALSE(invalid_state_flag.is_invalid());

  // With a pending current block already present, an additional insert should
  // not reserve another RCQ block in the upper bound.
  EXPECT_EQ(
      cost_model.kBaseItemCost + cost_model.kRCQCostPerEntry,
      variant.get_marginal_cost_upper_bound(
          make_rcq_delta(std::nullopt,
                         RetrocausalQueueInsertDelta{
                             .added_entries = {make_test_queue_entry(9)}}),
          &cost_model, item.get()));
}

TEST(RefcountedDeltaVariant, RCQBlocksRemainContiguousAfterPruneAndInsert)
{
  // Round 1: insert block 1
  RefcountedDeltaVariant round1; // NOLINT(misc-confusable-identifiers)
  InvalidStateFlag invalid_state_flag1;
  auto *arg1 = round1.try_set_variant<RetrocausalQueueArgs>();
  ASSERT_TRUE(arg1);
  arg1->commit_insert(
      RetrocausalQueueInsertDelta{.added_entries = {make_test_queue_entry(1)}},
      invalid_state_flag1);
  auto item1 = round1.to_item(nullptr, &cost_model);
  ASSERT_TRUE(item1);
  ASSERT_EQ(item1->queue().get_current_block_number(), 1U);

  // Round 2: insert block 2
  RefcountedDeltaVariant round2;
  auto *arg2 = round2.try_set_variant<RetrocausalQueueArgs>();
  ASSERT_TRUE(arg2);
  InvalidStateFlag invalid_state_flag2;
  arg2->commit_insert(
      RetrocausalQueueInsertDelta{.added_entries = {make_test_queue_entry(2)}},
      invalid_state_flag2);
  auto item2 = round2.to_item(item1, &cost_model);
  ASSERT_TRUE(item2);
  ASSERT_EQ(item2->queue().get_current_block_number(), 2U);

  // Round 3: consume block 1 fully, then insert one new entry.
  RefcountedDeltaVariant round3;
  auto *arg3 = round3.try_set_variant<RetrocausalQueueArgs>();
  ASSERT_TRUE(arg3);
  ASSERT_TRUE(arg3->try_reserve_consume());
  InvalidStateFlag invalid_state_flag3;
  arg3->commit_consume(
      RetrocausalQueueConsumeDelta{.past_block_consume =
                                       RetrocausalQueuePastBlockConsumeDelta{
                                           .max_fully_consumed_block_number = 1,
                                           .partial_consumed_number = 0},
                                   .current_block_consumed = {}},
      invalid_state_flag3);
  arg3->commit_insert(
      RetrocausalQueueInsertDelta{.added_entries = {make_test_queue_entry(3)}},
      invalid_state_flag3);
  auto item3 = round3.to_item(item2, &cost_model);
  ASSERT_TRUE(item3);

  auto const &queue = item3->queue();
  ASSERT_EQ(queue.get_current_block_number(), 3U);

  auto const *b2 = queue.try_get_next_block(1);
  ASSERT_TRUE(b2);
  EXPECT_EQ(b2->block_number, 2U);
  ASSERT_EQ(b2->entries.size(), 1U);
  EXPECT_EQ(b2->entries[0], make_test_queue_entry(2));

  auto const *b3 = queue.try_get_next_block(3);
  ASSERT_TRUE(b3);
  EXPECT_EQ(b3->block_number, 3U);
  ASSERT_EQ(b3->entries.size(), 1U);
  EXPECT_EQ(b3->entries[0], make_test_queue_entry(3));

  EXPECT_EQ(queue.try_get_next_block(4), nullptr);
}

TEST(RefcountedDeltaVariant, ReplayCacheFromEmpty)
{
  RefcountedDeltaVariant variant;
  auto *arg = variant.try_set_variant<ReplayCacheArgs>(nullptr, 0);
  ASSERT_TRUE(arg);

  EXPECT_TRUE(arg->try_reserve_insert(
      Uniquifier{.block_expiry_height = 1, .nonce = 1}));
  EXPECT_FALSE(arg->try_reserve_insert(
      Uniquifier{.block_expiry_height = 1, .nonce = 1}));

  EXPECT_TRUE(arg->try_reserve_insert(
      Uniquifier{.block_expiry_height = 1, .nonce = 2}));
  EXPECT_TRUE(arg->try_reserve_insert(
      Uniquifier{.block_expiry_height = 1, .nonce = 3}));

  arg->commit_insert(Uniquifier{.block_expiry_height = 1, .nonce = 1});
  arg->commit_insert(Uniquifier{.block_expiry_height = 1, .nonce = 2});
  arg->commit_insert(Uniquifier{.block_expiry_height = 1, .nonce = 3});

  EXPECT_EQ(variant.get_current_committed_cost(&cost_model, nullptr),
            cost_model.get_replay_cache_cost(3));

  auto item = variant.to_item(nullptr, &cost_model);

  ASSERT_EQ(item->type(), StorageItemType::kItemTypeReplayCacheData);
  auto const &cache = item->replay_cache();
  ASSERT_EQ(cache.uniquifiers().size(), 3);

  EXPECT_EQ(item->storage_cost(), cost_model.get_replay_cache_cost(3));

  RefcountedDeltaVariant variant2;
  auto *arg2 =
      variant2.try_set_variant<ReplayCacheArgs>(&item->replay_cache(), 2);
  ASSERT_TRUE(arg2);

  EXPECT_EQ(variant2.get_current_committed_cost(&cost_model, item.get()),
            cost_model.get_replay_cache_cost(3));

  ASSERT_TRUE(arg2->try_reserve_insert(
      Uniquifier{.block_expiry_height = 5, .nonce = 4}));

  arg2->commit_insert(Uniquifier{.block_expiry_height = 5, .nonce = 4});

  EXPECT_EQ(variant2.get_current_committed_cost(&cost_model, item.get()),
            cost_model.get_replay_cache_cost(1));
}

} // namespace hogd
