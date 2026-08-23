#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// NOLINTBEGIN(bugprone-unchecked-optional-access)
class DirectNonceTests : public ::testing::TestWithParam<TestDB>,
                         public NativeContractTestFramework {
public:
  DirectNonceTests()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

  void SetUp() override { cost_model_ = get_default_cost_model(); }

  StorageCostModel const *cost_model_;

  auto make_new_per_tx_db_proxy()
  {
    return NativeContractTestFramework::make_new_per_tx_db_proxy(UINT64_MAX);
  }

  static auto make_nonce_delta(Uniquifier nonce)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeConsumeUniquifier);
    delta.consume_uniquifier_args().uniquifier = nonce;
    return delta;
  }
};

TEST_P(DirectNonceTests, ConsumeConflicts)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockInMemory does not support replay cache";
  }
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();

  tx1.add_delta(make_replay_cache_addrkey(contract_addr),
                make_nonce_delta(
                    Uniquifier{.block_expiry_height = 100, .nonce = 100}),
                std::to_underlying(StorageDeltaFlags::None));

  auto r1 = tx1.try_push_current_subtransaction()();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  ASSERT_TRUE(tx1.commit_transaction()().has_value());

  auto tx2 = make_new_per_tx_db_proxy();

  tx2.add_delta(make_replay_cache_addrkey(contract_addr),
                make_nonce_delta(
                    Uniquifier{.block_expiry_height = 100, .nonce = 100}),
                std::to_underlying(StorageDeltaFlags::None));

  auto r2 = tx2.try_push_current_subtransaction()();
  ASSERT_TRUE(r2.has_value());
  ASSERT_FALSE(r2.value());

  ASSERT_TRUE(tx2.abort_transaction()().has_value());

  advance_block();

  auto r = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(r.has_value());
  EXPECT_NE(cost_model_->get_replay_cache_cost(1), 0);
  ASSERT_EQ(from_xdr(r.value().nnint()),
            1'000'000'000 - cost_model_->get_replay_cache_cost(1));
}

// Exclusive lock on ConsumeUniquifier should be rejected by both backends.
// Consuming a nonce is already a unique event, so there is no situation in
// which an honest executor sets ExclusiveLock on a replay cache delta: this is
// executor misbehavior, not a recoverable transaction conflict (issue #1080).
TEST_P(DirectNonceTests, ConsumeUniquifierExclusiveLockRejected)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(make_replay_cache_addrkey(contract_addr),
               make_nonce_delta(
                   Uniquifier{.block_expiry_height = 1000, .nonce = 42}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  auto r = tx.try_push_current_subtransaction()();
  ASSERT_FALSE(r.has_value())
      << "ConsumeUniquifier with exclusive lock should be rejected as "
         "executor misbehavior, not as a conflict";
  EXPECT_EQ(r.error().code(), SystemErrorCode::Unrecoverable);
  ASSERT_TRUE(tx.abort_transaction()().has_value());
}

TEST_P(DirectNonceTests,
       ReplayCacheGCStillRejectsPostExpiryConsumeWithoutExtraReservation)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockInMemory does not support replay cache";
  }

  auto contract_addr = make_contract_address(10);
  setup_grant_storage_token(contract_addr,
                            cost_model_->get_replay_cache_cost(2));

  auto commit_nonce_consume = [&](BlockHeight expiry_height,
                                  std::uint64_t nonce) {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_replay_cache_addrkey(contract_addr),
                 make_nonce_delta(
                     Uniquifier{.block_expiry_height = expiry_height,
                                .nonce = nonce}),
                 std::to_underlying(StorageDeltaFlags::None));

    auto push_result = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(push_result.has_value());
    ASSERT_TRUE(push_result.value());
    ASSERT_TRUE(tx.commit_transaction()().has_value());
  };

  // Blocks 1 and 2 each add a height-3 nonce.
  commit_nonce_consume(3, 100);
  advance_block();
  commit_nonce_consume(3, 200);
  advance_block();

  // Advance through empty blocks 3 and 4 so the next tx executes in block 5,
  // where the two height-3 entries are expired and should be GC'd.
  advance_block();
  advance_block();
  ASSERT_EQ(5, get_block_number());

  auto balance_before_gc = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(balance_before_gc.has_value());
  EXPECT_EQ(0, from_xdr(balance_before_gc->nnint()));

  auto post_expiry_tx = make_new_per_tx_db_proxy();
  post_expiry_tx.add_delta(make_replay_cache_addrkey(contract_addr),
                           make_nonce_delta(Uniquifier{.block_expiry_height = 7,
                                                       .nonce = 300}),
                           std::to_underlying(StorageDeltaFlags::None));

  auto post_expiry_push = post_expiry_tx.try_push_current_subtransaction()();
  ASSERT_TRUE(post_expiry_push.has_value());
  ASSERT_FALSE(post_expiry_push.value())
      << "The pre-GC reservation treats the cache size as 3 (current 2 + "
         "new 1), requiring balance for a third entry that is not available; "
         "the consume must be rejected at push time.";
  ASSERT_TRUE(post_expiry_tx.abort_transaction()().has_value());

  auto balance_after_rejection = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(balance_after_rejection.has_value());
  EXPECT_EQ(0, from_xdr(balance_after_rejection->nnint()));

  auto replay_meta =
      get_hog_db_dao().read_address_replay_cache_meta(contract_addr);
  ASSERT_TRUE(replay_meta);
  ASSERT_EQ(2, replay_meta->size);
  ASSERT_EQ(1, replay_meta->heights.size());
  EXPECT_EQ(3, replay_meta->heights[0].height);
  EXPECT_EQ(2, replay_meta->heights[0].nonce_count);
}

TEST_P(DirectNonceTests,
       ReplayCacheGCKeepsCorrectCommittedBalanceAcrossTwoTxsInSameBlock)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockInMemory does not support replay cache";
  }

  auto contract_addr = make_contract_address(11);
  const int128_t initial_balance = cost_model_->get_replay_cache_cost(3) +
                                   (2 * cost_model_->kReplayCacheCostPerEntry);
  setup_grant_storage_token(contract_addr, initial_balance);

  auto commit_nonce_consume = [&](BlockHeight expiry_height,
                                  std::uint64_t nonce) {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_replay_cache_addrkey(contract_addr),
                 make_nonce_delta(
                     Uniquifier{.block_expiry_height = expiry_height,
                                .nonce = nonce}),
                 std::to_underlying(StorageDeltaFlags::None));

    auto push_result = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(push_result.has_value());
    ASSERT_TRUE(push_result.value());
    ASSERT_TRUE(tx.commit_transaction()().has_value());
    advance_block();
  };

  // Seed 3 distinct expiry heights that all survive until block 7, where the
  // next replay-cache touch should GC all of them at once.
  commit_nonce_consume(4, 100);
  commit_nonce_consume(5, 200);
  commit_nonce_consume(6, 300);

  auto balance_before_gc_block = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(balance_before_gc_block.has_value());
  EXPECT_EQ(2 * cost_model_->kReplayCacheCostPerEntry,
            from_xdr(balance_before_gc_block->nnint()));

  advance_block();
  advance_block();
  advance_block();
  ASSERT_EQ(7, get_block_number());

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_replay_cache_addrkey(contract_addr),
                make_nonce_delta(
                    Uniquifier{.block_expiry_height = 7, .nonce = 400}),
                std::to_underlying(StorageDeltaFlags::None));

  auto tx1_push = tx1.try_push_current_subtransaction()();
  ASSERT_TRUE(tx1_push.has_value());
  ASSERT_TRUE(tx1_push.value());
  ASSERT_TRUE(tx1.commit_transaction()().has_value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_replay_cache_addrkey(contract_addr),
                make_nonce_delta(
                    Uniquifier{.block_expiry_height = 7, .nonce = 401}),
                std::to_underlying(StorageDeltaFlags::None));

  auto tx2_push = tx2.try_push_current_subtransaction()();
  ASSERT_TRUE(tx2_push.has_value());
  ASSERT_TRUE(tx2_push.value());
  ASSERT_TRUE(tx2.commit_transaction()().has_value());

  advance_block();

  auto final_balance = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(final_balance.has_value());
  EXPECT_EQ(initial_balance - cost_model_->get_replay_cache_cost(2),
            from_xdr(final_balance->nnint()));

  auto replay_meta =
      get_hog_db_dao().read_address_replay_cache_meta(contract_addr);
  ASSERT_TRUE(replay_meta);
  ASSERT_EQ(2, replay_meta->size);
  ASSERT_EQ(1, replay_meta->heights.size());
  EXPECT_EQ(7, replay_meta->heights[0].height);
  EXPECT_EQ(2, replay_meta->heights[0].nonce_count);
}

TEST_P(DirectNonceTests, ReplayCacheGCDoesNotRefundLaterAbortedReservation)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockInMemory does not support replay cache";
  }

  auto contract_addr = make_contract_address(12);
  const int128_t initial_balance = cost_model_->get_replay_cache_cost(3) +
                                   (2 * cost_model_->kReplayCacheCostPerEntry);
  setup_grant_storage_token(contract_addr, initial_balance);

  auto commit_nonce_consume = [&](BlockHeight expiry_height,
                                  std::uint64_t nonce) {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_replay_cache_addrkey(contract_addr),
                 make_nonce_delta(
                     Uniquifier{.block_expiry_height = expiry_height,
                                .nonce = nonce}),
                 std::to_underlying(StorageDeltaFlags::None));

    auto push_result = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(push_result.has_value());
    ASSERT_TRUE(push_result.value());
    ASSERT_TRUE(tx.commit_transaction()().has_value());
    advance_block();
  };

  commit_nonce_consume(4, 100);
  commit_nonce_consume(5, 200);
  commit_nonce_consume(6, 300);

  auto balance_before_gc_block = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(balance_before_gc_block.has_value());
  EXPECT_EQ(2 * cost_model_->kReplayCacheCostPerEntry,
            from_xdr(balance_before_gc_block->nnint()));

  advance_block();
  advance_block();
  advance_block();
  ASSERT_EQ(7, get_block_number());

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_replay_cache_addrkey(contract_addr),
                make_nonce_delta(
                    Uniquifier{.block_expiry_height = 7, .nonce = 400}),
                std::to_underlying(StorageDeltaFlags::None));

  auto tx1_push = tx1.try_push_current_subtransaction()();
  ASSERT_TRUE(tx1_push.has_value());
  ASSERT_TRUE(tx1_push.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_replay_cache_addrkey(contract_addr),
                make_nonce_delta(
                    Uniquifier{.block_expiry_height = 7, .nonce = 401}),
                std::to_underlying(StorageDeltaFlags::None));

  auto tx2_push = tx2.try_push_current_subtransaction()();
  ASSERT_TRUE(tx2_push.has_value());
  ASSERT_TRUE(tx2_push.value());

  ASSERT_TRUE(tx1.commit_transaction()().has_value());
  ASSERT_TRUE(tx2.abort_transaction()().has_value());

  advance_block();

  auto final_balance = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(final_balance.has_value());
  EXPECT_EQ(initial_balance - cost_model_->get_replay_cache_cost(1),
            from_xdr(final_balance->nnint()));

  auto replay_meta =
      get_hog_db_dao().read_address_replay_cache_meta(contract_addr);
  ASSERT_TRUE(replay_meta);
  ASSERT_EQ(1, replay_meta->size);
  ASSERT_EQ(1, replay_meta->heights.size());
  EXPECT_EQ(7, replay_meta->heights[0].height);
  EXPECT_EQ(1, replay_meta->heights[0].nonce_count);
}

TEST_P(DirectNonceTests, ReplayCacheGCHandlesThreeInBlockTxsWithMiddleAbort)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockInMemory does not support replay cache";
  }

  // Three concurrent in-block replay-cache additions where the middle tx
  // aborts. If the aborted tx's reservation somehow leaked into
  // committed_additions_, or if the two committed txs didn't properly
  // telescope off the post-GC baseline, the balance math would drift. With
  // the fix in place, commit_additions_ sees exactly 2, so the post-GC
  // charge is cost(2) rather than cost(3) or cost(1).
  auto contract_addr = make_contract_address(13);
  // Pre-GC reservations are cumulative and snapshot-based: with 3 concurrent
  // in-block additions, the third push needs cost(snapshot+3) in balance.
  const int128_t initial_balance = cost_model_->get_replay_cache_cost(3) +
                                   (3 * cost_model_->kReplayCacheCostPerEntry);
  setup_grant_storage_token(contract_addr, initial_balance);

  auto commit_nonce_consume = [&](BlockHeight expiry_height,
                                  std::uint64_t nonce) {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_replay_cache_addrkey(contract_addr),
                 make_nonce_delta(
                     Uniquifier{.block_expiry_height = expiry_height,
                                .nonce = nonce}),
                 std::to_underlying(StorageDeltaFlags::None));

    auto push_result = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(push_result.has_value());
    ASSERT_TRUE(push_result.value());
    ASSERT_TRUE(tx.commit_transaction()().has_value());
    advance_block();
  };

  // Seed three distinct heights that all expire by block 7.
  commit_nonce_consume(4, 100);
  commit_nonce_consume(5, 200);
  commit_nonce_consume(6, 300);

  auto balance_before_gc_block = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(balance_before_gc_block.has_value());
  EXPECT_EQ(3 * cost_model_->kReplayCacheCostPerEntry,
            from_xdr(balance_before_gc_block->nnint()));

  advance_block();
  advance_block();
  advance_block();
  ASSERT_EQ(7, get_block_number());

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_replay_cache_addrkey(contract_addr),
                make_nonce_delta(
                    Uniquifier{.block_expiry_height = 7, .nonce = 400}),
                std::to_underlying(StorageDeltaFlags::None));
  auto tx1_push = tx1.try_push_current_subtransaction()();
  ASSERT_TRUE(tx1_push.has_value());
  ASSERT_TRUE(tx1_push.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_replay_cache_addrkey(contract_addr),
                make_nonce_delta(
                    Uniquifier{.block_expiry_height = 7, .nonce = 401}),
                std::to_underlying(StorageDeltaFlags::None));
  auto tx2_push = tx2.try_push_current_subtransaction()();
  ASSERT_TRUE(tx2_push.has_value());
  ASSERT_TRUE(tx2_push.value());

  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_replay_cache_addrkey(contract_addr),
                make_nonce_delta(
                    Uniquifier{.block_expiry_height = 7, .nonce = 402}),
                std::to_underlying(StorageDeltaFlags::None));
  auto tx3_push = tx3.try_push_current_subtransaction()();
  ASSERT_TRUE(tx3_push.has_value());
  ASSERT_TRUE(tx3_push.value());

  // tx2 is the middle tx that aborts.

  ASSERT_TRUE(tx1.commit_transaction()().has_value());
  ASSERT_TRUE(tx2.abort_transaction()().has_value());
  ASSERT_TRUE(tx3.commit_transaction()().has_value());

  advance_block();

  // Final committed size is 2 (tx1 and tx3 survived), so balance is
  // initial - cost(2): the snapshot cost(3) is refunded on the first commit
  // and tx3 pays the single incremental
  // cost(2)-cost(1)=kReplayCacheCostPerEntry.
  auto final_balance = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(final_balance.has_value());
  EXPECT_EQ(initial_balance - cost_model_->get_replay_cache_cost(2),
            from_xdr(final_balance->nnint()));

  auto replay_meta =
      get_hog_db_dao().read_address_replay_cache_meta(contract_addr);
  ASSERT_TRUE(replay_meta);
  ASSERT_EQ(2, replay_meta->size);
  ASSERT_EQ(1, replay_meta->heights.size());
  EXPECT_EQ(7, replay_meta->heights[0].height);
  EXPECT_EQ(2, replay_meta->heights[0].nonce_count);
}

TEST_P(DirectNonceTests, ReplayCacheHeightCapRejects4001stHeight)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockInMemory does not support replay cache";
  }

  auto contract_addr = make_contract_address(42);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Pre-populate the DAO with exactly kReplayCacheHeightsMaxLen active
  // heights for this address. Going through the runtime for 4000 txs would be
  // prohibitively slow; the bug under test is in the lock/DAO layer, and the
  // trigger tx below is what actually exercises the end-to-end path.
  auto &dao = get_hog_db_dao();
  constexpr BlockHeight kSeedBase = 1000;

  ReplayCacheMetadata meta;
  meta.size = kReplayCacheHeightsMaxLen;
  meta.heights.reserve(kReplayCacheHeightsMaxLen);
  for (std::int64_t i = 0; i < kReplayCacheHeightsMaxLen; ++i) {
    BlockHeight h = kSeedBase + static_cast<BlockHeight>(i);
    meta.heights.push_back(
        ReplayCacheHeightCount{.height = h, .nonce_count = 1});

    ReplayCacheData data;
    data.nonces.push_back(static_cast<int64_t>(h));
    ASSERT_TRUE(dao.write_address_replay_cache_data(contract_addr, h, data));
  }
  ASSERT_TRUE(dao.write_address_replay_cache_meta(contract_addr, meta));

  // Drive the trigger tx through the full runtime. Pick a block_expiry_height
  // strictly greater than every seeded height and greater than the current
  // block number so expiry checks do not fire.
  const BlockHeight new_height =
      kSeedBase + static_cast<BlockHeight>(kReplayCacheHeightsMaxLen) + 1000;

  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(make_replay_cache_addrkey(contract_addr),
               make_nonce_delta(
                   Uniquifier{.block_expiry_height = new_height, .nonce = 77}),
               std::to_underlying(StorageDeltaFlags::None));

  auto r = tx.try_push_current_subtransaction()();
  ASSERT_TRUE(r.has_value());
  ASSERT_FALSE(r.value())
      << "replay-cache height cap at " << kReplayCacheHeightsMaxLen
      << " must reject a brand-new height once the cap is reached";

  ASSERT_TRUE(tx.abort_transaction()().has_value());

  // Metadata must still be exactly at the cap, not one over.
  auto final_meta = dao.read_address_replay_cache_meta(contract_addr);
  ASSERT_TRUE(final_meta);
  EXPECT_EQ(static_cast<std::int64_t>(kReplayCacheHeightsMaxLen),
            static_cast<std::int64_t>(final_meta->heights.size()));
}

// Regression for #2581: expired heights must not permanently consume the
// active-height budget. Genuinely fill the replay cache to the height cap in a
// single block, advance one block so the lowest height expires, then verify a
// brand-new height can still be inserted -- the freed (expired) slot must be
// reclaimable. Pre-fix, get_base() counted the expired height against the cap
// and rejected the insert, leaving the cache permanently locked out.
TEST_P(DirectNonceTests, ExpiredHeightFreesCapSlotForNewHeightNextBlock)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockInMemory does not support replay cache";
  }

  auto contract_addr = make_contract_address(43);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto consume = [&](BlockHeight expiry, std::uint64_t nonce) {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_replay_cache_addrkey(contract_addr),
                 make_nonce_delta(
                     Uniquifier{.block_expiry_height = expiry, .nonce = nonce}),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    if (!r.has_value() || !r.value()) {
      return std::make_pair(r.has_value(), r.has_value() && r.value());
    }
    EXPECT_TRUE(tx.commit_transaction()().has_value());
    return std::make_pair(true, true);
  };

  // Block 0: genuinely consume kReplayCacheHeightsMaxLen uniquifiers, each at a
  // distinct expiry height, filling the active-height budget exactly to the
  // cap. The lowest height equals the current block so it expires next block.
  const auto base_height = static_cast<BlockHeight>(get_block_number());
  for (std::int64_t i = 0; i < kReplayCacheHeightsMaxLen; ++i) {
    const BlockHeight expiry = base_height + static_cast<BlockHeight>(i);
    auto [ok, admitted] = consume(expiry, static_cast<std::uint64_t>(i));
    ASSERT_TRUE(ok);
    ASSERT_TRUE(admitted) << "height index " << i << " must fit under the cap";
  }

  // One more distinct height in the SAME block exceeds the cap and is rejected.
  {
    const BlockHeight over_cap =
        base_height + static_cast<BlockHeight>(kReplayCacheHeightsMaxLen);
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_replay_cache_addrkey(contract_addr),
                 make_nonce_delta(Uniquifier{.block_expiry_height = over_cap,
                                             .nonce = 9999}),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    ASSERT_FALSE(r.value())
        << "the " << (kReplayCacheHeightsMaxLen + 1)
        << "th distinct height in one block must exceed the cap";
    ASSERT_TRUE(tx.abort_transaction()().has_value());
  }

  // Advance one block: the lowest seeded height (base_height) is now expired,
  // freeing exactly one active-height slot.
  advance_block();
  ASSERT_EQ(base_height + 1, get_block_number());

  // A fresh height must now be admitted: the expired height must not count
  // against the cap.
  const BlockHeight fresh_height =
      base_height + static_cast<BlockHeight>(kReplayCacheHeightsMaxLen) + 1;
  auto [ok, admitted] = consume(fresh_height, 12345);
  ASSERT_TRUE(ok);
  ASSERT_TRUE(admitted)
      << "an expired height freed a slot; the fresh height must be admitted "
         "(#2581)";

  advance_block();

  // The expired base_height row was GC'd at block close. The cache holds the
  // 3999 still-live seeded heights plus the freshly inserted one == the cap.
  auto meta = get_hog_db_dao().read_address_replay_cache_meta(contract_addr);
  ASSERT_TRUE(meta);
  EXPECT_EQ(static_cast<std::int64_t>(kReplayCacheHeightsMaxLen),
            static_cast<std::int64_t>(meta->heights.size()));
  EXPECT_EQ(base_height + 1, meta->heights.front().height);
  EXPECT_EQ(fresh_height, meta->heights.back().height);
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, DirectNonceTests,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

} // namespace hogd
