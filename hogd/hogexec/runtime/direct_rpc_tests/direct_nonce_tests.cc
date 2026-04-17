#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
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
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP() << "MockInMemory does not support replay cache";
  }
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();

  tx1.add_delta(make_replay_cache_addrkey(contract_addr),
                make_nonce_delta(
                    Uniquifier{.block_expiry_height = 100, .nonce = 100}),
                std::to_underlying(StorageDeltaFlags::None));

  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  auto tx2 = make_new_per_tx_db_proxy();

  tx2.add_delta(make_replay_cache_addrkey(contract_addr),
                make_nonce_delta(
                    Uniquifier{.block_expiry_height = 100, .nonce = 100}),
                std::to_underlying(StorageDeltaFlags::None));

  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_FALSE(r2.value());

  ASSERT_TRUE(tx2.abort_transaction().has_value());

  advance_block();

  auto r = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(r.has_value());
  EXPECT_NE(cost_model_->get_replay_cache_cost(1), 0);
  ASSERT_EQ(from_xdr(r.value().nnint()),
            1'000'000'000 - cost_model_->get_replay_cache_cost(1));
}

// Exclusive lock on ConsumeUniquifier should be rejected by both backends.
TEST_P(DirectNonceTests, ConsumeUniquifierExclusiveLockRejected)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(make_replay_cache_addrkey(contract_addr),
               make_nonce_delta(
                   Uniquifier{.block_expiry_height = 1000, .nonce = 42}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value());
  EXPECT_FALSE(r.value())
      << "ConsumeUniquifier with exclusive lock should be rejected";
  ASSERT_TRUE(tx.abort_transaction().has_value());
  advance_block();
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

    auto push_result = tx.try_push_current_subtransaction();
    ASSERT_TRUE(push_result.has_value());
    ASSERT_TRUE(push_result.value());
    ASSERT_TRUE(tx.commit_transaction({}).has_value());
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

  auto post_expiry_push = post_expiry_tx.try_push_current_subtransaction();
  ASSERT_TRUE(post_expiry_push.has_value());
  ASSERT_FALSE(post_expiry_push.value())
      << "The pre-GC reservation treats the cache size as 3 (current 2 + "
         "new 1), requiring balance for a third entry that is not available; "
         "the consume must be rejected at push time.";
  ASSERT_TRUE(post_expiry_tx.abort_transaction().has_value());

  auto balance_after_rejection = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(balance_after_rejection.has_value());
  EXPECT_EQ(0, from_xdr(balance_after_rejection->nnint()));

  auto replay_meta =
      get_hogdb_dao().read_address_replay_cache_meta(contract_addr);
  ASSERT_TRUE(replay_meta);
  ASSERT_EQ(2, replay_meta->size);
  ASSERT_EQ(1, replay_meta->heights.size());
  EXPECT_EQ(3, replay_meta->heights[0].height);
  EXPECT_EQ(2, replay_meta->heights[0].nonce_count);
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, DirectNonceTests,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
