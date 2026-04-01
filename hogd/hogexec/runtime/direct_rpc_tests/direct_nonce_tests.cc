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
  tx.add_delta(
      make_replay_cache_addrkey(contract_addr),
      make_nonce_delta(Uniquifier{.block_expiry_height = 1000, .nonce = 42}),
      std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value());
  EXPECT_FALSE(r.value())
      << "ConsumeUniquifier with exclusive lock should be rejected";
  ASSERT_TRUE(tx.abort_transaction().has_value());
  advance_block();
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, DirectNonceTests,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
