#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <set>
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
#include "types.hh"
#include "x23/vec.h"

namespace hogd {
namespace {
QueueEntry
make_test_queue_entry(uint8_t value)
{
  return {value};
}

const StorageCostModel cost_model(12000, 1, 5, 100, 5);

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

namespace {
StorageDelta
make_replay_cache_delta(x23::Vec<ReplayNonce> nonces,
                        ReplayNonce minimum_threshold)
{
  StorageDelta delta(StorageDeltaType::kDeltaTypeReplayCacheModify);
  delta.replay_cache_modify().nonces = std::move(nonces);
  delta.replay_cache_modify().minimum_threshold = minimum_threshold;
  return delta;
}

} // namespace

TEST(RefcountedDeltaVariant, ReplayCacheFromEmpty)
{
  RefcountedDeltaVariant variant;
  auto *arg = variant.try_set_variant<ReplayCacheArgs>(nullptr);
  ASSERT_TRUE(arg);

  EXPECT_TRUE(arg->try_reserve_insert(1));
  EXPECT_FALSE(arg->try_reserve_insert(1));

  EXPECT_TRUE(arg->try_reserve_insert(3));
  EXPECT_TRUE(arg->try_reserve_insert(2));

  // Clearing a replay cache immediately doesn't charge for storage of the cleared item.
  // The inequalities account for the fact that we might or might not charge marginal storage cost
  // for inserting and immediately clearing the item (this doesn't affect final storage costs,
  // just complexity of the marginal calculation)
  EXPECT_EQ(variant.get_marginal_cost_upper_bound(make_replay_cache_delta({1, 2, 3}, 0),
  &cost_model, nullptr),
            cost_model.kBaseItemCost + cost_model.get_replay_cache_cost(3));
  EXPECT_GE(variant.get_marginal_cost_upper_bound(make_replay_cache_delta({1, 2, 3}, 1),
                                          &cost_model, nullptr),
            cost_model.kBaseItemCost + cost_model.get_replay_cache_cost(2));
  EXPECT_GE(variant.get_marginal_cost_upper_bound(make_replay_cache_delta({1, 2, 3}, 2),
                                          &cost_model, nullptr),
            cost_model.kBaseItemCost + cost_model.get_replay_cache_cost(1));
  EXPECT_GE(variant.get_marginal_cost_upper_bound(make_replay_cache_delta({1, 2, 3}, 3),
                                          &cost_model, nullptr),
            cost_model.kBaseItemCost + cost_model.get_replay_cache_cost(0));

  for (ReplayNonce i = 1; i <= 3; i++) {
    arg->commit_insert(i);
  }
  arg->commit_threshold_change(2);

  EXPECT_EQ(variant.get_current_committed_cost(&cost_model, nullptr),
            cost_model.kBaseItemCost + cost_model.get_replay_cache_cost(1));

  auto item = variant.to_item(nullptr, &cost_model);

  ASSERT_EQ(item->type(), StorageItemType::kItemTypeReplayCache);
  auto const &cache = item->replay_cache();
  ASSERT_EQ(cache.metadata().minimum_threshold, 2);
  ASSERT_EQ(cache.nonces().size(), 1);
  ASSERT_EQ(cache.nonces().count(1), 0);
  ASSERT_EQ(cache.nonces().count(2), 0);
  ASSERT_EQ(cache.nonces().count(3), 1);
  EXPECT_EQ(item->storage_cost(),
            cost_model.kBaseItemCost + cost_model.get_replay_cache_cost(1));
}

TEST(RefcountedDeltaVariant, ReplayCacheFromNonEmpty)
{
  const ReplayCache base_cache(ReplayCacheMetadata{.minimum_threshold = 10},
                               {11, 12, 14, 100, 1000});
  auto base_item = std::make_shared<const StorageItemWrapper>(base_cache, &cost_model);

  RefcountedDeltaVariant variant;
  auto *arg = variant.try_set_variant<ReplayCacheArgs>(&base_item->replay_cache());
  ASSERT_TRUE(arg);

  EXPECT_FALSE(arg->try_reserve_insert(10));
  EXPECT_FALSE(arg->try_reserve_insert(11));
  EXPECT_FALSE(arg->try_reserve_insert(12));
  EXPECT_TRUE(arg->try_reserve_insert(13));
  EXPECT_FALSE(arg->try_reserve_insert(14));

  EXPECT_EQ(variant.get_marginal_cost_upper_bound(make_replay_cache_delta({13}, 0),
                                          &cost_model, base_item.get()),
            cost_model.get_replay_cache_cost(1));

  // Clearing can't reduce your marginal cost upper bound -- that's released at commit time.
  EXPECT_EQ(variant.get_marginal_cost_upper_bound(make_replay_cache_delta({13}, 13),
                                          &cost_model, base_item.get()),
            cost_model.get_replay_cache_cost(1));

  arg->commit_threshold_change(12);
  arg->commit_insert(13);

  auto item = variant.to_item(nullptr, &cost_model);

  ASSERT_EQ(item->type(), StorageItemType::kItemTypeReplayCache);
  auto const &cache = item->replay_cache();
  ASSERT_EQ(cache.metadata().minimum_threshold, 12);
  ASSERT_EQ(cache.nonces().size(), 4);
  std::set<ReplayNonce> expected_nonces = {13, 14, 100, 1000};
  ASSERT_EQ(cache.nonces(), expected_nonces);
  EXPECT_EQ(item->storage_cost(),
            cost_model.kBaseItemCost + cost_model.get_replay_cache_cost(4));
}

} // namespace hogd
