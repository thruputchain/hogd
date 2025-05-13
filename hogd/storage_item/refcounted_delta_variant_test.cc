#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/storage_item/invalid_state_flag.h"
#include "hogd/storage_item/refcounted_delta_variant.h"
#include "hogd/storage_item/retrocausal_queue.h"
#include "hogd/storage_item/storage_costs.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "storage_lock.hh"

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

TEST(RefcountedDeltaVariant, AssetBalanceProposal)
{
  AssetBalanceArgs arg(0);

  auto r = arg.try_reserve_transfer_lock(
      100, AssetBalanceLock{.asset_add_amount = to_xdr(int128_t{100}),
                            .asset_subtract_amount = to_xdr(int128_t{0})});
  ASSERT_TRUE(r);
  EXPECT_EQ(r->asset_add_amount, to_xdr(int128_t{100}));
  EXPECT_EQ(r->asset_subtract_amount, to_xdr(int128_t{0}));

  r = arg.try_reserve_transfer_lock(100, *r);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->asset_add_amount, to_xdr(int128_t{100}));
  EXPECT_EQ(r->asset_subtract_amount, to_xdr(int128_t{0}));

  r = arg.try_reserve_transfer_lock(50, *r);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->asset_add_amount, to_xdr(int128_t{100}));
  EXPECT_EQ(r->asset_subtract_amount, to_xdr(int128_t{0}));

  r = arg.try_reserve_transfer_lock(200, *r);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->asset_add_amount, to_xdr(int128_t{200}));
  EXPECT_EQ(r->asset_subtract_amount, to_xdr(int128_t{0}));

  ASSERT_FALSE(arg.try_reserve_transfer_lock(-100, *r));
}

TEST(RefcountedDeltaVariant, AssetBalanceProposalLargeValues)
{
  AssetBalanceArgs arg(std::numeric_limits<uint128_t>::max() - 5);

  auto r = arg.try_reserve_transfer_lock(
      100, AssetBalanceLock{.asset_add_amount = to_xdr(int128_t{0}),
                            .asset_subtract_amount = to_xdr(int128_t{0})});
  ASSERT_FALSE(r);

  r = arg.try_reserve_transfer_lock(
      5, AssetBalanceLock{.asset_add_amount = to_xdr(int128_t{0}),
                          .asset_subtract_amount = to_xdr(int128_t{0})});
  ASSERT_TRUE(r);
  EXPECT_EQ(r->asset_add_amount, to_xdr(int128_t{5}));
  EXPECT_EQ(r->asset_subtract_amount, to_xdr(int128_t{0}));
}

TEST(RefcountedDeltaVariant, AssetBalanceProposalNegative)
{
  AssetBalanceArgs arg(500);

  auto r = arg.try_reserve_transfer_lock(
      -100, AssetBalanceLock{.asset_add_amount = to_xdr(int128_t{0}),
                             .asset_subtract_amount = to_xdr(int128_t{0})});
  ASSERT_TRUE(r);
  EXPECT_EQ(r->asset_add_amount, to_xdr(int128_t{0}));
  EXPECT_EQ(r->asset_subtract_amount, to_xdr(int128_t{-100}));

  r = arg.try_reserve_transfer_lock(-100, *r);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->asset_add_amount, to_xdr(int128_t{0}));
  EXPECT_EQ(r->asset_subtract_amount, to_xdr(int128_t{-100}));

  r = arg.try_reserve_transfer_lock(100, *r);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->asset_add_amount, to_xdr(int128_t{100}));
  EXPECT_EQ(r->asset_subtract_amount, to_xdr(int128_t{-100}));

  r = arg.try_reserve_transfer_lock(-50, *r);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->asset_add_amount, to_xdr(int128_t{100}));
  EXPECT_EQ(r->asset_subtract_amount, to_xdr(int128_t{-100}));

  r = arg.try_reserve_transfer_lock(-400, *r);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->asset_add_amount, to_xdr(int128_t{100}));
  EXPECT_EQ(r->asset_subtract_amount, to_xdr(int128_t{-400}));

  ASSERT_FALSE(arg.try_reserve_transfer_lock(-501, *r));
}

TEST(RefcountedDeltaVariant, AssetBalanceProposalConcurrentDeltas)
{
  AssetBalanceArgs arg(100);

  // reserve an add
  auto r1 = arg.try_reserve_transfer_lock(
      100, AssetBalanceLock{.asset_add_amount = to_xdr(int128_t{0}),
                            .asset_subtract_amount = to_xdr(int128_t{0})});
  ASSERT_TRUE(r1);
  EXPECT_EQ(r1->asset_add_amount, to_xdr(int128_t{100}));
  EXPECT_EQ(r1->asset_subtract_amount, to_xdr(int128_t{0}));

  // reserve a subtract, that prevents all other subtracts
  auto r2 = arg.try_reserve_transfer_lock(
      -100, AssetBalanceLock{.asset_add_amount = to_xdr(int128_t{0}),
                             .asset_subtract_amount = to_xdr(int128_t{0})});
  ASSERT_TRUE(r2);
  EXPECT_EQ(r2->asset_add_amount, to_xdr(int128_t{0}));
  EXPECT_EQ(r2->asset_subtract_amount, to_xdr(int128_t{-100}));

  // more adds are still fine
  r1 = arg.try_reserve_transfer_lock(200, *r1);
  ASSERT_TRUE(r1);
  EXPECT_EQ(r1->asset_add_amount, to_xdr(int128_t{200}));
  EXPECT_EQ(r1->asset_subtract_amount, to_xdr(int128_t{0}));

  // more subtracts are not fine
  ASSERT_FALSE(arg.try_reserve_transfer_lock(-1, *r1));
}

TEST(RefcountedDeltaVariant, AssetBalanceProposalConcurrentDeltasRevert)
{
  AssetBalanceArgs arg(200);
  InvalidStateFlag invalid_state_flag;
  // Step 1: reserve an add of 100
  auto r1 = arg.try_reserve_transfer_lock(
      100, AssetBalanceLock{.asset_add_amount = to_xdr(int128_t{0}),
                            .asset_subtract_amount = to_xdr(int128_t{0})});
  ASSERT_TRUE(r1);
  EXPECT_EQ(r1->asset_add_amount, to_xdr(int128_t{100}));
  EXPECT_EQ(r1->asset_subtract_amount, to_xdr(int128_t{0}));

  // Step 2: reserve a subtract of 100, from the same lock (r1)
  auto r1b = arg.try_reserve_transfer_lock(-100, *r1);
  ASSERT_TRUE(r1b);
  EXPECT_EQ(r1b->asset_add_amount, to_xdr(int128_t{100}));
  EXPECT_EQ(r1b->asset_subtract_amount, to_xdr(int128_t{-100}));

  // Step 3: Reserve a subtract of 50, from a new lock (r2)
  auto r2 = arg.try_reserve_transfer_lock(
      -50, AssetBalanceLock{.asset_add_amount = to_xdr(int128_t{0}),
                            .asset_subtract_amount = to_xdr(int128_t{0})});
  ASSERT_TRUE(r2);
  EXPECT_EQ(r2->asset_add_amount, to_xdr(int128_t{0}));
  EXPECT_EQ(r2->asset_subtract_amount, to_xdr(int128_t{-50}));

  // Step 4: Assert that another subtract of 51 from a new lock is not possible
  ASSERT_FALSE(arg.try_reserve_transfer_lock(
      -51, AssetBalanceLock{.asset_add_amount = to_xdr(int128_t{0}),
                            .asset_subtract_amount = to_xdr(int128_t{0})}));
  // Additionally impossible from r1b
  ASSERT_FALSE(arg.try_reserve_transfer_lock(-151, *r1b));

  // Step 5: Revert r2
  arg.revert_lock(*r2);

  // Step 6: Assert that a subtract of 151 from r1b is possible, to get lock r3
  // (Current lock state should allow another subtract of 100, as r2 is
  // released)
  auto r3 = arg.try_reserve_transfer_lock(-151, *r1b);
  ASSERT_TRUE(r3);
  EXPECT_EQ(r3->asset_add_amount, to_xdr(int128_t{100}));
  EXPECT_EQ(r3->asset_subtract_amount, to_xdr(int128_t{-151}));

  // Step 7: commit transfer of 155 based on lock r3
  arg.commit_transfer(155, *r3, invalid_state_flag);
  ASSERT_FALSE(invalid_state_flag.is_invalid());

  auto item = arg.to_item();
  auto balance = item->asset_balance();
  EXPECT_EQ(balance, 355);
}

TEST(RefcountedDeltaVariant, AssetBalanceValidationOutOfBounds)
{
  AssetBalanceArgs arg(100);
  InvalidStateFlag invalid_state_flag;

  arg.commit_transfer(-100,
                      AssetBalanceLock{.asset_add_amount = to_xdr(int128_t{0}),
                                       .asset_subtract_amount =
                                           to_xdr(int128_t{0})},
                      invalid_state_flag);
  ASSERT_FALSE(invalid_state_flag.is_invalid());

  arg.commit_transfer(-1,
                      AssetBalanceLock{.asset_add_amount = to_xdr(int128_t{0}),
                                       .asset_subtract_amount =
                                           to_xdr(int128_t{0})},
                      invalid_state_flag);
  ASSERT_TRUE(invalid_state_flag.is_invalid());

  arg.commit_transfer(-5,
                      AssetBalanceLock{.asset_add_amount = to_xdr(int128_t{0}),
                                       .asset_subtract_amount =
                                           to_xdr(int128_t{0})},
                      invalid_state_flag);
  ASSERT_TRUE(invalid_state_flag.is_invalid());

  arg.commit_transfer(6,
                      AssetBalanceLock{.asset_add_amount = to_xdr(int128_t{0}),
                                       .asset_subtract_amount =
                                           to_xdr(int128_t{0})},
                      invalid_state_flag);
  ASSERT_FALSE(invalid_state_flag.is_invalid());
}

} // namespace hogd
