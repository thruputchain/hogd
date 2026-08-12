#include <gtest/gtest.h>

#include <cstdint>
#include <print>
#include <string>
#include <string_view>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

using namespace std::string_literals;

// NOLINTBEGIN(bugprone-unchecked-optional-access)

class DirectBytestringTest : public ::testing::TestWithParam<TestDB>,
                             public NativeContractTestFramework {
public:
  DirectBytestringTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

  void SetUp() override { cost_model_ = get_default_cost_model(); }

  static auto make_addrkey(ContractAddress contract_addr, std::string_view key)
  {
    return make_contract_data_addrkey(contract_addr, key);
  }

  // check that the two addresses have the same balance,
  // and that that balance is less than or equal to
  // the initial balance
  void check_balances(ContractAddress contract_addr,
                      ContractAddress reference_addr, int128_t expected_balance)
  {
    // check that costs are reflected in the balance
    auto bal = lookup_storage_item_from_db(
        make_free_native_token_balance_addrkey(contract_addr));
    auto bal_ref = lookup_storage_item_from_db(
        make_free_native_token_balance_addrkey(reference_addr));
    ASSERT_TRUE(bal.has_value());
    ASSERT_TRUE(bal_ref.has_value());
    int128_t bal_int = from_xdr(bal.value().nnint());
    int128_t bal_ref_int = from_xdr(bal_ref.value().nnint());

    EXPECT_EQ(bal_int, bal_ref_int);
    EXPECT_EQ(bal_int, expected_balance);
  }

  auto make_new_per_tx_db_proxy()
  {
    return NativeContractTestFramework::make_new_per_tx_db_proxy(UINT64_MAX);
  }

  const StorageCostModel *cost_model_;
};

TEST_P(DirectBytestringTest, ConcurrentWrites)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // 3 identical writes ok, different write bad
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  auto tx_diff = make_new_per_tx_db_proxy();
  tx_diff.add_delta(make_addrkey(contract_addr, "test"),
                    make_bytestring_delta("different_value"),
                    std::to_underlying(StorageDeltaFlags::None));
  auto r_diff = tx_diff.try_push_current_subtransaction();
  ASSERT_TRUE(r_diff.has_value());
  ASSERT_FALSE(r_diff.value());

  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r3 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value());
}

TEST_P(DirectBytestringTest, BytestringStorageCosts)
{
  auto contract_addr1 = make_contract_address(0);
  auto contract_addr2 = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr1, kInitialBalance);
  setup_grant_storage_token(contract_addr2, kInitialBalance);

  // Creating the bstring several times should have same cost as creating it
  // once
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr1, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  auto tx1_1 = make_new_per_tx_db_proxy();
  tx1_1.add_delta(make_addrkey(contract_addr1, "test"),
                  make_bytestring_delta("value"),
                  std::to_underlying(StorageDeltaFlags::None));
  auto r1_1 = tx1_1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_1.has_value());
  ASSERT_TRUE(r1_1.value());

  auto tx1_2 = make_new_per_tx_db_proxy();
  tx1_2.add_delta(make_addrkey(contract_addr1, "test"),
                  make_bytestring_delta("value"),
                  std::to_underlying(StorageDeltaFlags::None));
  auto r1_2 = tx1_2.try_push_current_subtransaction();
  ASSERT_TRUE(r1_2.has_value());
  ASSERT_TRUE(r1_2.value());

  // create the same bytestring on a different contract
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr2, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  ASSERT_TRUE(tx1.commit_transaction().has_value());
  ASSERT_TRUE(tx1_1.commit_transaction().has_value());
  ASSERT_TRUE(tx1_2.commit_transaction().has_value());
  ASSERT_TRUE(tx2.commit_transaction().has_value());

  advance_block();
  check_balances(contract_addr1, contract_addr2,
                 kInitialBalance -
                     cost_model_->get_bytestring_cost("value"s.length()));
}

TEST_P(DirectBytestringTest, RevertStorageCosts)
{
  // Create bytestring twice, but first one reverts.
  // The bytestring is still created, and so the cost should reflect
  // one bytestring.

  auto contract_addr = make_contract_address(0);
  auto reference_addr = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;

  setup_grant_storage_token(contract_addr, kInitialBalance);
  setup_grant_storage_token(reference_addr, kInitialBalance);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  ASSERT_TRUE(tx1.abort_transaction().has_value());
  ASSERT_TRUE(tx2.commit_transaction().has_value());

  // compare with single tx address
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_addrkey(reference_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r3 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value());
  ASSERT_TRUE(tx3.commit_transaction().has_value());

  advance_block();

  check_balances(contract_addr, reference_addr,
                 kInitialBalance -
                     cost_model_->get_bytestring_cost("value"s.length()));
}

TEST_P(DirectBytestringTest, LengthIncreaseCost)
{
  auto contract_addr = make_contract_address(0);
  auto reference_addr = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);
  setup_grant_storage_token(reference_addr, kInitialBalance);

  auto tx_init = make_new_per_tx_db_proxy();
  tx_init.add_delta(make_addrkey(contract_addr, "test"),
                    make_bytestring_delta("value"),
                    std::to_underlying(StorageDeltaFlags::None));
  auto r_init = tx_init.try_push_current_subtransaction();
  ASSERT_TRUE(r_init.has_value());
  ASSERT_TRUE(r_init.value());

  ASSERT_TRUE(tx_init.commit_transaction().has_value());
  advance_block();

  // Test begins:
  // Create a bytestring of longer length, which costs more.
  // Three txs create this string, but the first and the third revert.

  auto make_test_call = [&]() {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_addrkey(contract_addr, "test"),
                 make_bytestring_delta("much_longer_value"),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction();
    EXPECT_TRUE(r.has_value());
    EXPECT_TRUE(r.value());
    return tx;
  };

  auto tx1 = make_test_call();
  auto tx2 = make_test_call();
  auto tx3 = make_test_call();
  ASSERT_TRUE(tx1.abort_transaction().has_value());
  ASSERT_TRUE(tx2.commit_transaction().has_value());
  ASSERT_TRUE(tx3.abort_transaction().has_value());

  // reference address
  auto tx_ref = make_new_per_tx_db_proxy();
  tx_ref.add_delta(make_addrkey(reference_addr, "test"),
                   make_bytestring_delta("much_longer_value"),
                   std::to_underlying(StorageDeltaFlags::None));
  auto r_ref = tx_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_ref.has_value());
  ASSERT_TRUE(r_ref.value());
  ASSERT_TRUE(tx_ref.commit_transaction().has_value());

  advance_block();
  check_balances(contract_addr, reference_addr,
                 kInitialBalance - cost_model_->get_bytestring_cost(
                                       "much_longer_value"s.length()));
}

TEST_P(DirectBytestringTest, DecreaseLengthCost)
{
  auto contract_addr = make_contract_address(0);
  auto reference_addr = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);
  setup_grant_storage_token(reference_addr, kInitialBalance);

  auto tx_init = make_new_per_tx_db_proxy();
  tx_init.add_delta(make_addrkey(contract_addr, "test"),
                    make_bytestring_delta("long_initial_value"),
                    std::to_underlying(StorageDeltaFlags::None));
  auto r_init = tx_init.try_push_current_subtransaction();
  ASSERT_TRUE(r_init.has_value());
  ASSERT_TRUE(r_init.value());
  ASSERT_TRUE(tx_init.commit_transaction().has_value());
  advance_block();

  // Test begins:
  // Create a bytestring of shorter length, which costs less.
  // Three txs create this string, but the first and the third revert.

  auto make_test_call = [&]() {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_addrkey(contract_addr, "test"),
                 make_bytestring_delta("v"), // short value
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction();
    EXPECT_TRUE(r.has_value());
    return tx;
  };

  auto tx1 = make_test_call();
  auto tx2 = make_test_call();
  auto tx3 = make_test_call();
  ASSERT_TRUE(tx1.abort_transaction().has_value());
  ASSERT_TRUE(tx2.commit_transaction().has_value());
  ASSERT_TRUE(tx3.abort_transaction().has_value());

  // reference address
  auto tx_ref = make_new_per_tx_db_proxy();
  tx_ref.add_delta(make_addrkey(reference_addr, "test"),
                   make_bytestring_delta("v"),
                   std::to_underlying(StorageDeltaFlags::None));
  auto r_ref = tx_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_ref.has_value());
  ASSERT_TRUE(r_ref.value());
  ASSERT_TRUE(tx_ref.commit_transaction().has_value());

  advance_block();
  check_balances(contract_addr, reference_addr,
                 kInitialBalance -
                     cost_model_->get_bytestring_cost("v"s.length()));
}

TEST_P(DirectBytestringTest, DecreaseLengthCostAllCommit)
{
  auto contract_addr = make_contract_address(0);
  auto reference_addr = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);
  setup_grant_storage_token(reference_addr, kInitialBalance);

  auto tx_init = make_new_per_tx_db_proxy();
  tx_init.add_delta(make_addrkey(contract_addr, "test"),
                    make_bytestring_delta("long_initial_value"),
                    std::to_underlying(StorageDeltaFlags::None));
  auto r_init = tx_init.try_push_current_subtransaction();
  ASSERT_TRUE(r_init.has_value());
  ASSERT_TRUE(r_init.value());
  ASSERT_TRUE(tx_init.commit_transaction().has_value());
  advance_block();

  // Test begins:
  // Create a bytestring of shorter length, which costs less.
  // Three txs create this string, and all commit.

  auto make_test_call = [&]() {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_addrkey(contract_addr, "test"),
                 make_bytestring_delta("v"), // short value
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction();
    EXPECT_TRUE(r.has_value());
    return tx;
  };

  auto tx1 = make_test_call();
  auto tx2 = make_test_call();
  auto tx3 = make_test_call();
  ASSERT_TRUE(tx1.commit_transaction().has_value());
  ASSERT_TRUE(tx2.commit_transaction().has_value());
  ASSERT_TRUE(tx3.commit_transaction().has_value());

  // reference address
  auto tx_ref = make_new_per_tx_db_proxy();
  tx_ref.add_delta(make_addrkey(reference_addr, "test"),
                   make_bytestring_delta("v"),
                   std::to_underlying(StorageDeltaFlags::None));
  auto r_ref = tx_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_ref.has_value());
  ASSERT_TRUE(r_ref.value());
  ASSERT_TRUE(tx_ref.commit_transaction().has_value());

  advance_block();
  check_balances(contract_addr, reference_addr,
                 kInitialBalance -
                     cost_model_->get_bytestring_cost("v"s.length()));
}

TEST_P(DirectBytestringTest, DeletionCost)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto tx_init = make_new_per_tx_db_proxy();
  tx_init.add_delta(make_addrkey(contract_addr, "test"),
                    make_bytestring_delta("value"),
                    std::to_underlying(StorageDeltaFlags::None));
  auto r_init = tx_init.try_push_current_subtransaction();
  ASSERT_TRUE(r_init.has_value());
  ASSERT_TRUE(r_init.value());
  ASSERT_TRUE(tx_init.commit_transaction().has_value());
  advance_block();

  // Test begins:
  // Delete the bytestring
  auto make_test_call = [&]() {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction();
    EXPECT_TRUE(r.has_value());
    return tx;
  };

  auto tx1 = make_test_call();
  auto tx2 = make_test_call();
  auto tx3 = make_test_call();
  ASSERT_TRUE(tx1.abort_transaction().has_value());
  ASSERT_TRUE(tx2.commit_transaction().has_value());
  ASSERT_TRUE(tx3.commit_transaction().has_value());

  advance_block();

  auto bal1 = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));

  ASSERT_TRUE(bal1.has_value());
  EXPECT_EQ(from_xdr(bal1.value().nnint()), kInitialBalance);
}

TEST_P(DirectBytestringTest, TryInvokeWithDelete)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto tx_init = make_new_per_tx_db_proxy();
  tx_init.add_delta(make_addrkey(contract_addr, "test"),
                    make_bytestring_delta("value"),
                    std::to_underlying(StorageDeltaFlags::None));
  auto r_init = tx_init.try_push_current_subtransaction();
  ASSERT_TRUE(r_init.has_value());
  ASSERT_TRUE(r_init.value());
  ASSERT_TRUE(tx_init.commit_transaction().has_value());
  advance_block();

  // Start the test

  // tx1 deletes the key
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  // tx2 deletes the key
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  // tx1 deletes the key again
  tx1.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1_1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_1.has_value());
  ASSERT_TRUE(r1_1.value());

  // tx2 aborts
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  // tx1 commits
  ASSERT_TRUE(tx1.commit_transaction().has_value());

  advance_block();

  auto bal1 = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));

  ASSERT_TRUE(bal1.has_value());
  EXPECT_EQ(from_xdr(bal1.value().nnint()), kInitialBalance);
}

TEST_P(DirectBytestringTest, TryInvokeMultipleTimes)
{
  auto contract_addr = make_contract_address(0);
  auto reference_addr = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);
  setup_grant_storage_token(reference_addr, kInitialBalance);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_longer"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_even_longer"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r3 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value());

  ASSERT_TRUE(tx1.pop_subtransactions(1).has_value());
  ASSERT_TRUE(tx1.commit_transaction().has_value());

  auto tx_ref = make_new_per_tx_db_proxy();
  tx_ref.add_delta(make_addrkey(reference_addr, "test"),
                   make_bytestring_delta("value_longer"),
                   std::to_underlying(StorageDeltaFlags::None));
  auto r_ref = tx_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_ref.has_value());
  ASSERT_TRUE(r_ref.value());
  ASSERT_TRUE(tx_ref.commit_transaction().has_value());

  advance_block();
  check_balances(contract_addr, reference_addr,
                 kInitialBalance - cost_model_->get_bytestring_cost(
                                       "value_longer"s.length()));
}

TEST_P(DirectBytestringTest, TryInvokeMultipleTimesShorter)
{
  auto contract_addr = make_contract_address(0);
  auto reference_addr = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);
  setup_grant_storage_token(reference_addr, kInitialBalance);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_even_longer"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_longer"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r3 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value());

  ASSERT_TRUE(tx1.pop_subtransactions(1).has_value());
  ASSERT_TRUE(tx1.commit_transaction().has_value());

  auto tx_ref = make_new_per_tx_db_proxy();
  tx_ref.add_delta(make_addrkey(reference_addr, "test"),
                   make_bytestring_delta("value_longer"),
                   std::to_underlying(StorageDeltaFlags::None));
  auto r_ref = tx_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_ref.has_value());
  ASSERT_TRUE(r_ref.value());
  ASSERT_TRUE(tx_ref.commit_transaction().has_value());

  advance_block();
  check_balances(contract_addr, reference_addr,
                 kInitialBalance - cost_model_->get_bytestring_cost(
                                       "value_longer"s.length()));
}

TEST_P(DirectBytestringTest, TryInvokeReleaseExclusiveLock)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_longer"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1_1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_1.has_value());
  ASSERT_TRUE(r1_1.value());

  // now tx1 holds the exclusive lock

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_longer"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_FALSE(r2.value()); // should fail because tx1 holds the exclusive lock

  ASSERT_TRUE(tx1.commit_transaction().has_value());

  // now the exclusive lock is no longer held
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_longer"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2_1 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_1.has_value());
  ASSERT_TRUE(r2_1.value());

  // but tx2 cannot write a different value
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_even_longer"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2_2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_2.has_value());
  ASSERT_FALSE(r2_2.value());

  ASSERT_TRUE(tx2.commit_transaction().has_value());
  advance_block();

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  auto item = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_addr, "test"));
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(item.value().bytestring(), make_bytestring("value_longer"));
  // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_P(DirectBytestringTest, TryInvokeWithRegular)
{
  auto contract_addr = make_contract_address(0);
  auto reference_addr = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);
  setup_grant_storage_token(reference_addr, kInitialBalance);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_longer"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1_1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_1.has_value());
  ASSERT_TRUE(r1_1.value());

  ASSERT_TRUE(tx1.commit_transaction().has_value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_longer"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  ASSERT_TRUE(tx2.commit_transaction().has_value());

  auto tx_ref = make_new_per_tx_db_proxy();
  tx_ref.add_delta(make_addrkey(reference_addr, "test"),
                   make_bytestring_delta("value_longer"),
                   std::to_underlying(StorageDeltaFlags::None));
  auto r_ref = tx_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_ref.has_value());
  ASSERT_TRUE(r_ref.value());
  ASSERT_TRUE(tx_ref.commit_transaction().has_value());

  advance_block();
  check_balances(contract_addr, reference_addr,
                 kInitialBalance - cost_model_->get_bytestring_cost(
                                       "value_longer"s.length()));
}

TEST_P(DirectBytestringTest, TryInvokeWithRegular2)
{
  auto contract_addr = make_contract_address(0);
  auto reference_addr = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);
  setup_grant_storage_token(reference_addr, kInitialBalance);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_longer"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1_1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_1.has_value());
  ASSERT_FALSE(r1_1.value());

  ASSERT_TRUE(tx1.commit_transaction().has_value());
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  // reference: the first bytestring write is what commits
  auto tx_ref = make_new_per_tx_db_proxy();
  tx_ref.add_delta(make_addrkey(reference_addr, "test"),
                   make_bytestring_delta("value"),
                   std::to_underlying(StorageDeltaFlags::None));
  auto r_ref = tx_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_ref.has_value());
  ASSERT_TRUE(r_ref.value());
  ASSERT_TRUE(tx_ref.commit_transaction().has_value());

  advance_block();
  check_balances(contract_addr, reference_addr,
                 kInitialBalance -
                     cost_model_->get_bytestring_cost("value"s.length()));
}

TEST_P(DirectBytestringTest, TransferKeyCosts)
{
  auto contract_addr = make_contract_address(0);
  auto reference_addr = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);
  setup_grant_storage_token(reference_addr, kInitialBalance);

  auto tx_init = make_new_per_tx_db_proxy();
  tx_init.add_delta(make_addrkey(contract_addr, "test"),
                    make_bytestring_delta("value"),
                    std::to_underlying(StorageDeltaFlags::None));
  auto r_init = tx_init.try_push_current_subtransaction();
  ASSERT_TRUE(r_init.has_value());
  ASSERT_TRUE(r_init.value());
  ASSERT_TRUE(tx_init.commit_transaction().has_value());

  auto tx_init_ref = make_new_per_tx_db_proxy();
  tx_init_ref.add_delta(make_addrkey(reference_addr, "test_new"),
                        make_bytestring_delta("value"),
                        std::to_underlying(StorageDeltaFlags::None));
  auto r_init_ref = tx_init_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_init_ref.has_value());
  ASSERT_TRUE(r_init_ref.value());
  ASSERT_TRUE(tx_init_ref.commit_transaction().has_value());
  advance_block();

  // start the test

  // tx2: just delete the key
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  // tx1: Creates the new key, then deletes the old one, in one operation
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test_new"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  tx1.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  ASSERT_TRUE(tx1.commit_transaction().has_value());
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  advance_block();
  check_balances(contract_addr, reference_addr,
                 kInitialBalance -
                     cost_model_->get_bytestring_cost("value"s.length()));
}

TEST_P(DirectBytestringTest, DeletionCostNotImmediatelyAvailable)
{
  if (GetParam() == TestDB::kHogDb) {
    GTEST_SKIP() << "Pending hog_db retrocausal nnint";
    return;
  }
  auto contract_addr = make_contract_address(0);
  auto reference_addr = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(reference_addr, kInitialBalance);

  auto tx_ref_init = make_new_per_tx_db_proxy();
  tx_ref_init.add_delta(make_addrkey(reference_addr, "test_new"),
                        make_bytestring_delta("value"),
                        std::to_underlying(StorageDeltaFlags::None));
  auto r_init = tx_ref_init.try_push_current_subtransaction();
  ASSERT_TRUE(r_init.has_value());
  ASSERT_TRUE(r_init.value());
  ASSERT_TRUE(tx_ref_init.commit_transaction().has_value());
  advance_block();

  auto ref_bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(reference_addr));
  ASSERT_TRUE(ref_bal.has_value());
  int128_t ref_bal_int = from_xdr(ref_bal.value().nnint());
  EXPECT_LT(ref_bal_int, kInitialBalance);
  int128_t required_balance = kInitialBalance - ref_bal_int;
  setup_grant_storage_token(contract_addr, required_balance);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  ASSERT_TRUE(tx1.commit_transaction().has_value());
  advance_block();
  auto initial_bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(initial_bal.has_value());
  int128_t initial_bal_int = from_xdr(initial_bal.value().nnint());
  EXPECT_EQ(initial_bal_int, 0);

  // Now the test begins.

  // tx2: just delete the key
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  // tx3: create a new key
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_addrkey(contract_addr, "test_new"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r3 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_FALSE(r3.value()); // storage not yet available

  ASSERT_TRUE(tx2.commit_transaction().has_value());

  // now storage is available
  // This is where hog_db doesn't allow a write, when a retrocausal nnint would
  // allow it.
  tx3.add_delta(make_addrkey(contract_addr, "test_new"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r3_1 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3_1.has_value());
  ASSERT_TRUE(r3_1.value());

  ASSERT_TRUE(tx3.commit_transaction().has_value());

  advance_block();
  auto bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(bal.has_value());
  int128_t bal_int = from_xdr(bal.value().nnint());
  EXPECT_EQ(bal_int, 0);
}

TEST_P(DirectBytestringTest, DeletionCostsNotReleasedWhenRollbackPossible)
{
  // This is basically the same test as DeletionCostNotImmediatelyAvailable,
  // but without the last check that the storage becomes available after the
  // commit.
  auto contract_addr = make_contract_address(0);
  auto reference_addr = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(reference_addr, kInitialBalance);

  auto tx_ref_init = make_new_per_tx_db_proxy();
  tx_ref_init.add_delta(make_addrkey(reference_addr, "test_new"),
                        make_bytestring_delta("value"),
                        std::to_underlying(StorageDeltaFlags::None));
  auto r_init = tx_ref_init.try_push_current_subtransaction();
  ASSERT_TRUE(r_init.has_value());
  ASSERT_TRUE(r_init.value());
  ASSERT_TRUE(tx_ref_init.commit_transaction().has_value());
  advance_block();

  auto ref_bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(reference_addr));
  ASSERT_TRUE(ref_bal.has_value());
  int128_t ref_bal_int = from_xdr(ref_bal.value().nnint());
  EXPECT_LT(ref_bal_int, kInitialBalance);
  int128_t required_balance = kInitialBalance - ref_bal_int;
  setup_grant_storage_token(contract_addr, required_balance);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  ASSERT_TRUE(tx1.commit_transaction().has_value());
  advance_block();
  auto initial_bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(initial_bal.has_value());
  int128_t initial_bal_int = from_xdr(initial_bal.value().nnint());
  EXPECT_EQ(initial_bal_int, 0);

  // Now the test begins.

  // tx2: just delete the key
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  std::println("tx3 cannot use the storage");
  // tx3: create a new key
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_addrkey(contract_addr, "test_new"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r3 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_FALSE(r3.value()); // storage not yet available

  // Note that neither can TX2 use the storage.
  // We don't know, prior to committing, whether the storage free'd up by
  // the delete will belong to this tx (or will there be some other tx that also
  // deletes the key).  In the future, we could make the storage available
  // CONDITIONAL on the delete having the kExclusive flag set.
  tx2.add_delta(make_addrkey(contract_addr, "test_new"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2_1 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_1.has_value());
  ASSERT_FALSE(r2_1.value());

  ASSERT_TRUE(tx2.commit_transaction().has_value());
  ASSERT_TRUE(tx3.abort_transaction().has_value());

  advance_block();
  auto bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(bal.has_value());
  int128_t bal_int = from_xdr(bal.value().nnint());
  EXPECT_EQ(bal_int, required_balance);
}

TEST_P(DirectBytestringTest, RollbackPartwayThroughSavepoint)
{

  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto tx_block = make_new_per_tx_db_proxy();
  tx_block.add_delta(make_addrkey(contract_addr, "test"),
                     make_bytestring_delta("value"),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r_block = tx_block.try_push_current_subtransaction();
  ASSERT_TRUE(r_block.has_value());
  ASSERT_TRUE(r_block.value());
  ASSERT_TRUE(tx_block.commit_transaction().has_value());

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test_nonblock"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_different"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_FALSE(r1.value());

  ASSERT_TRUE(tx1.abort_transaction().has_value());
}

TEST_P(DirectBytestringTest, DeleteThenSetConflict)
{
  auto contract_addr = make_contract_address(0);
  auto reference_addr = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);
  setup_grant_storage_token(reference_addr, kInitialBalance);

  // Prepopulate key
  {
    auto tx_init = make_new_per_tx_db_proxy();
    tx_init.add_delta(make_addrkey(contract_addr, "test"),
                      make_bytestring_delta("value"),
                      std::to_underlying(StorageDeltaFlags::None));
    auto r_init = tx_init.try_push_current_subtransaction();
    ASSERT_TRUE(r_init.has_value());
    ASSERT_TRUE(r_init.value());
    ASSERT_TRUE(tx_init.commit_transaction().has_value());
    advance_block();
  }

  // Start conflict scenario: two deletes, then one tries to set
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  // Attempt to set a new value in tx1 while tx2 still holds a lock
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("new_value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1_set = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_set.has_value());
  ASSERT_FALSE(r1_set.value()); // cannot upgrade under shared lock in mockdb

  ASSERT_TRUE(tx1.commit_transaction().has_value()); // deletes the key
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  // Reference: just a single delete on another address
  auto tx_ref = make_new_per_tx_db_proxy();
  tx_ref.add_delta(make_addrkey(reference_addr, "test"), make_delete_delta(),
                   std::to_underlying(StorageDeltaFlags::None));
  auto r_ref = tx_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_ref.has_value());
  // If key absent, delete is treated as no-op but allowed in mockdb path
  ASSERT_TRUE(tx_ref.commit_transaction().has_value());

  advance_block();
  check_balances(contract_addr, reference_addr, kInitialBalance);
}

TEST_P(DirectBytestringTest, DeleteMissingKeyNoOp)
{

  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  // No prior write to this key; deletion should be a no-op.
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(make_addrkey(contract_addr, "missing_key"), make_delete_delta(),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(r.value());
  ASSERT_TRUE(tx.commit_transaction().has_value());

  advance_block();

  auto bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(bal.has_value());
  EXPECT_EQ(from_xdr(bal.value().nnint()), kInitialBalance);
}

TEST_P(DirectBytestringTest, DoubleReleaseLowerCost)
{

  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(make_addrkey(contract_addr, "test"),
               make_bytestring_delta("value_long"),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(r.value());
  ASSERT_TRUE(tx.commit_transaction().has_value());

  advance_block();

  // setup done

  auto bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(bal.has_value());
  EXPECT_EQ(from_xdr(bal.value().nnint()),
            kInitialBalance -
                cost_model_->get_bytestring_cost("value_long"s.length()));

  // Now for the test
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r3 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value());

  ASSERT_TRUE(tx2.commit_transaction().has_value());
  ASSERT_TRUE(tx3.commit_transaction().has_value());

  advance_block();

  bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(bal.has_value());
  EXPECT_EQ(from_xdr(bal.value().nnint()),
            kInitialBalance -
                cost_model_->get_bytestring_cost("value"s.length()));
}

TEST_P(DirectBytestringTest, TxLocalReleaseCosts)
{

  auto contract_addr = make_contract_address(0);
  const int128_t initial_cost =
      cost_model_->get_bytestring_cost("value_long"s.length()) +
      cost_model_->get_bytestring_cost("short"s.length());
  setup_grant_storage_token(contract_addr, initial_cost);

  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(make_addrkey(contract_addr, "test1"),
               make_bytestring_delta("short"),
               std::to_underlying(StorageDeltaFlags::None));
  tx.add_delta(make_addrkey(contract_addr, "test2"),
               make_bytestring_delta("short"),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(r.value());
  ASSERT_TRUE(tx.commit_transaction().has_value());

  advance_block();

  auto bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(bal.has_value());
  EXPECT_EQ(from_xdr(bal.value().nnint()),
            cost_model_->get_bytestring_cost("value_long"s.length()) -
                cost_model_->get_bytestring_cost("short"s.length()));

  // We're going to try to release costs from test1 to use on test2.
  // We (1) first acquire a lock on writing value_long to test1.
  // We (2) then try to write value_long to test2, which should fail due to
  // storage costs. We (3) then write s to test1, which should locally release
  // locked costs. We (4) then try to write value_long to test2, which should
  // succeed. We (5) then try to write value_longer to test2, which should fail.
  // (We can't release storage costs that we didn't acquire during the tx)
  // After committing, we should have
  // get_bytestring_cost("value_long"s.length()) -
  // get_bytestring_cost("s"s.length()) cost available.

  // step 1
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test1"),
                make_bytestring_delta("value_long"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  // step 2
  tx2.add_delta(make_addrkey(contract_addr, "test2"),
                make_bytestring_delta("value_long"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2_1 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_1.has_value());
  ASSERT_FALSE(r2_1.value());

  // step 3
  tx2.add_delta(make_addrkey(contract_addr, "test1"),
                make_bytestring_delta("s"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2_2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_2.has_value());
  ASSERT_TRUE(r2_2.value());

  // step 4
  tx2.add_delta(make_addrkey(contract_addr, "test2"),
                make_bytestring_delta("value_long"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2_3 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_3.has_value());
  ASSERT_TRUE(r2_3.value());

  // step 5
  tx2.add_delta(make_addrkey(contract_addr, "test2"),
                make_bytestring_delta("value_longer"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2_4 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_4.has_value());
  ASSERT_FALSE(r2_4.value());

  ASSERT_TRUE(tx2.commit_transaction().has_value());

  advance_block();

  const int128_t expected_cost =
      cost_model_->get_bytestring_cost("value_long"s.length()) +
      cost_model_->get_bytestring_cost("s"s.length());

  bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(bal.has_value());
  EXPECT_EQ(from_xdr(bal.value().nnint()), initial_cost - expected_cost);

  auto item1 =
      lookup_storage_item_from_db(make_addrkey(contract_addr, "test1"));
  ASSERT_TRUE(item1.has_value());
  EXPECT_EQ(item1.value().bytestring(), make_bytestring("s"));

  auto item2 =
      lookup_storage_item_from_db(make_addrkey(contract_addr, "test2"));
  ASSERT_TRUE(item2.has_value());
  EXPECT_EQ(item2.value().bytestring(), make_bytestring("value_long"));
}

TEST_P(DirectBytestringTest, RollbackInfallibilityGivenLocalCostRelease)
{
  auto contract_addr = make_contract_address(0);
  const int128_t initial_cost =
      2 * cost_model_->get_bytestring_cost("value_long"s.length());
  setup_grant_storage_token(contract_addr, initial_cost);

  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(make_addrkey(contract_addr, "test1"),
               make_bytestring_delta("value"),
               std::to_underlying(StorageDeltaFlags::None));
  tx.add_delta(make_addrkey(contract_addr, "test2"),
               make_bytestring_delta("value"),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(r.value());
  ASSERT_TRUE(tx.commit_transaction().has_value());

  advance_block();

  // done setup

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test1"),
                make_bytestring_delta("value_long"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  tx2.add_delta(make_addrkey(contract_addr, "test1"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2_1 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_1.has_value());
  ASSERT_TRUE(r2_1.value());

  // tx2 has now locked an "_long" worth of storage costs, but is not using them
  // for the top level value. It still needs them to maintain rollback
  // potential.

  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_addrkey(contract_addr, "test2"),
                make_bytestring_delta("value_long"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r3 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value());

  // This fails, because tx2 has the costs locked.
  tx3.add_delta(make_addrkey(contract_addr, "test2"),
                make_bytestring_delta("value_long2"), // slightly longer
                std::to_underlying(StorageDeltaFlags::None));
  auto r3_1 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3_1.has_value());
  ASSERT_FALSE(r3_1.value());

  ASSERT_TRUE(tx2.commit_transaction().has_value());

  // now the costs should be released, although hog_db does not implement this
  // yet
  //
  // tx3.add_delta(make_addrkey(contract_addr, "test2"),
  // make_bytestring_delta("value_long2"), // slightly longer
  // std::to_underlying(StorageDeltaFlags::None));

  ASSERT_TRUE(tx3.commit_transaction().has_value());

  advance_block();
}

TEST_P(DirectBytestringTest, ReuseExclusiveLock)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value2"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1_1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_1.has_value());
  ASSERT_TRUE(r1_1.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value3"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1_2 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_2.has_value());
  ASSERT_TRUE(r1_2.value());

  ASSERT_TRUE(tx1.commit_transaction().has_value());

  advance_block();

  auto val = lookup_storage_item_from_db(make_addrkey(contract_addr, "test"));
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value().bytestring(), make_bytestring("value3"));
}

TEST_P(DirectBytestringTest, ReusePermamentExclusiveLock)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value2"),
                std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  auto r1_1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_1.has_value());
  ASSERT_TRUE(r1_1.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value3"),
                std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  auto r1_2 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_2.has_value());
  ASSERT_TRUE(r1_2.value());

  ASSERT_TRUE(tx1.commit_transaction().has_value());

  advance_block();

  auto val = lookup_storage_item_from_db(make_addrkey(contract_addr, "test"));
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value().bytestring(), make_bytestring("value3"));
}

TEST_P(DirectBytestringTest, LockAfterPermanentExclusiveLock)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());
  ASSERT_TRUE(tx1.commit_transaction().has_value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_FALSE(r2.value());
  ASSERT_TRUE(tx2.commit_transaction().has_value());

  advance_block();

  auto val = lookup_storage_item_from_db(make_addrkey(contract_addr, "test"));
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value().bytestring(), make_bytestring("value"));
}

TEST_P(DirectBytestringTest, PermamentLockAfterRegularLock)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());
  ASSERT_TRUE(tx1.commit_transaction().has_value());

  // can't acquire an exclusive lock after the regular lock has been committed
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_FALSE(r2.value());
  ASSERT_TRUE(tx2.commit_transaction().has_value());

  advance_block();

  auto val = lookup_storage_item_from_db(make_addrkey(contract_addr, "test"));
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value().bytestring(), make_bytestring("value"));
}

TEST_P(DirectBytestringTest, LockChangeAfterCommit)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());
  ASSERT_TRUE(tx1.commit_transaction().has_value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));

  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value2"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2_1 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_1.has_value());
  ASSERT_FALSE(r2_1.value());
  ASSERT_TRUE(tx2.commit_transaction().has_value());
  advance_block();

  auto val = lookup_storage_item_from_db(make_addrkey(contract_addr, "test"));
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value().bytestring(), make_bytestring("value"));
}

TEST_P(DirectBytestringTest, PermanentExclusiveLockReleaseCostsDeleteItem)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB doesn't implement permanent exclusive lock "
                    "immediate cost release";
    return;
  }
  auto contract_addr = make_contract_address(0);
  const int128_t initial_cost =
      cost_model_->get_bytestring_cost("value"s.length());
  setup_grant_storage_token(contract_addr, initial_cost);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());
  ASSERT_TRUE(tx1.commit_transaction().has_value());

  advance_block();

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  tx2.add_delta(make_addrkey(contract_addr, "test2"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());
  ASSERT_TRUE(tx2.commit_transaction().has_value());

  advance_block();

  auto bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(bal.has_value());
  EXPECT_EQ(from_xdr(bal.value().nnint()), 0);
}

TEST_P(DirectBytestringTest, PermanentExclusiveLockReacquireAfterTryInvoke)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB doesn't implement permanent exclusive lock "
                    "immediate cost release";
    return;
  }
  auto contract_addr = make_contract_address(0);
  const int128_t initial_cost =
      cost_model_->get_bytestring_cost("value"s.length());
  setup_grant_storage_token(contract_addr, initial_cost);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());
  ASSERT_TRUE(tx1.commit_transaction().has_value());

  advance_block();

  // This first try_invoke succeeds, because the permanent exclusive lock
  // releases the storage costs of the deleted item.

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  tx2.add_delta(make_addrkey(contract_addr, "test2"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  // This fails, because changing this to a normal change forces us to reacquire
  // the storage costs.
  tx2.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2_1 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_1.has_value());
  ASSERT_FALSE(r2_1.value());

  // This succeeds, because we stop the test2 write
  tx2.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::None));
  tx2.add_delta(make_addrkey(contract_addr, "test2"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2_2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_2.has_value());
  ASSERT_TRUE(r2_2.value());

  ASSERT_TRUE(tx2.commit_transaction().has_value());

  // The write was made without an exclusive lock, so another transaction should
  // be able to delete the item

  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::None));
  auto r3 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value());
  ASSERT_TRUE(tx3.commit_transaction().has_value());

  advance_block();

  auto bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(bal.has_value());
  EXPECT_EQ(from_xdr(bal.value().nnint()), initial_cost);
}

TEST_P(DirectBytestringTest,
       PermanentExclusiveLockReacquireAfterTryInvokeChangingBytestringLength)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB doesn't implement permanent exclusive lock "
                    "immediate cost release";
    return;
  }
  auto contract_addr = make_contract_address(0);
  const int128_t initial_cost =
      cost_model_->get_bytestring_cost("value"s.length()) +
      cost_model_->get_bytestring_cost("value_long"s.length());
  setup_grant_storage_token(contract_addr, initial_cost);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  tx1.add_delta(make_addrkey(contract_addr, "test2"),
                make_bytestring_delta("value_long"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());
  ASSERT_TRUE(tx1.commit_transaction().has_value());

  advance_block();
  // done test setup

  // This fails, because the increase in storage cost is not performed while the
  // exclusive lock is held.

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_long"),
                std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  tx2.add_delta(make_addrkey(contract_addr, "test2"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_FALSE(r2.value());

  // This succeeds, because the decrease in storage cost is performed while the
  // exclusive lock is held.
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_long"),
                std::to_underlying(StorageDeltaFlags::None));
  tx2.add_delta(make_addrkey(contract_addr, "test2"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  auto r2_1 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_1.has_value());
  ASSERT_TRUE(r2_1.value());

  // This fails, because this downgrades the lock on test2 to a regular lock,
  // while insufficient storage costs are available.
  tx2.add_delta(make_addrkey(contract_addr, "test2"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2_2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_2.has_value());
  ASSERT_FALSE(r2_2.value());

  // This succeeds, because the change on test releases the storage costs that
  // were acquired in this tx.
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  tx2.add_delta(make_addrkey(contract_addr, "test2"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2_3 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_3.has_value());
  ASSERT_TRUE(r2_3.value());

  ASSERT_TRUE(tx2.commit_transaction().has_value());

  // The write was made without an exclusive lock, so another transaction should
  // be able to write to the item

  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  tx3.add_delta(make_addrkey(contract_addr, "test2"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r3 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value());
  ASSERT_TRUE(tx3.commit_transaction().has_value());

  advance_block();
}

TEST_P(DirectBytestringTest, MultipleDeletes)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB doesn't implement permanent exclusive lock "
                    "immediate cost release";
    return;
  }
  auto contract_addr = make_contract_address(0);
  const int128_t initial_cost =
      cost_model_->get_bytestring_cost("value"s.length());
  setup_grant_storage_token(contract_addr, initial_cost);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());
  ASSERT_TRUE(tx1.commit_transaction().has_value());

  advance_block();
  // test setup done

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::None));
  auto r3 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value());

  // this lock fails because tx3 already has deleted the item.
  tx2.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  auto r2_1 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_1.has_value());
  ASSERT_FALSE(r2_1.value());

  ASSERT_TRUE(tx3.abort_transaction().has_value());

  // now tx2 can acquire in exclusive mode
  tx2.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  tx2.add_delta(make_addrkey(contract_addr, "test2"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  auto r2_2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_2.has_value());
  ASSERT_TRUE(r2_2.value());

  ASSERT_TRUE(tx2.commit_transaction().has_value());

  advance_block();

  auto bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(bal.has_value());
  EXPECT_EQ(from_xdr(bal.value().nnint()), 0);
}

// Ensure that behavior before and after a concurrent delta is consistent
TEST_P(DirectBytestringTest, BugHuntNNIntAddOnBytestringKeyAfterConcurrentDelta)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto key = make_addrkey(contract_addr, "nnint_on_bs");

  // Create bytestring
  {
    auto tx_init = make_new_per_tx_db_proxy();
    tx_init.add_delta(key, make_bytestring_delta("existing"),
                      std::to_underlying(StorageDeltaFlags::None));
    auto r = tx_init.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
    ASSERT_TRUE(tx_init.commit_transaction().has_value());
    advance_block();
  }

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_nnint_set_add_delta(0, 10),
                std::to_underlying(StorageDeltaFlags::None));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value() && r1.value());
  ASSERT_TRUE(tx1.commit_transaction().has_value());

  // Try NNInt add on the bytestring key
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key, make_nnint_add_delta(10),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx.commit_transaction().has_value());
  advance_block();

  auto item = lookup_storage_item_from_db(key);
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(item->type(), StorageItemType::kItemTypeNonnegativeInt);
  EXPECT_EQ(from_xdr(item->nnint()), 20);
}

TEST_P(DirectBytestringTest, BugHuntNNIntAddOnBytestringKey)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto key = make_addrkey(contract_addr, "nnint_on_bs");

  // Create bytestring
  {
    auto tx_init = make_new_per_tx_db_proxy();
    tx_init.add_delta(key, make_bytestring_delta("existing"),
                      std::to_underlying(StorageDeltaFlags::None));
    auto r = tx_init.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
    ASSERT_TRUE(tx_init.commit_transaction().has_value());
    advance_block();
  }

  // Try NNInt add on the bytestring key
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key, make_nnint_add_delta(10),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx.commit_transaction().has_value());
  advance_block();

  auto item = lookup_storage_item_from_db(key);
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(item->type(), StorageItemType::kItemTypeNonnegativeInt);
  EXPECT_EQ(from_xdr(item->nnint()), 10);
}

TEST_P(DirectBytestringTest, BugHuntNNIntAddOnExistingBytestringNoExcl)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);
  auto addrkey = make_addrkey(contract_addr, "bs_then_nnint_add");

  // Create key as bytestring.
  {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(addrkey, make_bytestring_delta("existing_value"),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
    ASSERT_TRUE(tx.commit_transaction().has_value());
    advance_block();
  }

  // Now NNInt add-only on that key (no exclusive lock).
  {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(addrkey, make_nnint_add_delta(100),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());

    ASSERT_TRUE(tx.commit_transaction().has_value());
  }
  advance_block();

  auto item = lookup_storage_item_from_db(addrkey);
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(item->type(), StorageItemType::kItemTypeNonnegativeInt);
  EXPECT_EQ(from_xdr(item->nnint()), 100);
}

TEST_P(DirectBytestringTest, BugHuntLargeBytestringExhaustsBalance)
{
  auto contract_addr = make_contract_address(0);
  // Small balance that can't cover a large bytestring.
  // 1024 bytes costs 12000 + 10*1024 = 22240. Give 20000 which is insufficient.
  setup_grant_storage_token(contract_addr, 20'000);
  auto addrkey = make_addrkey(contract_addr, "large_bs");

  // Vec<unsigned char, 1024> has max capacity of 1024, so use that.
  std::string large_str(1024,
                        'X'); // 1024 bytes -> cost = 12000 + 10240 = 22240

  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(addrkey, make_bytestring_delta(large_str),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && !r.value());
}

// A subtransaction whose delta batch would not fit the executor->HogDb RPC
// message is rejected as a concurrency-control conflict (a clean `false`, not a
// system error), so an oversized push can never reach serialization and become
// a node-unrecoverable error. This is the general r/w-footprint bound; here it
// is driven by spraying max-size bytestrings across many keys (the same check
// covers many-keys and RCQ-observation overflow).
TEST_P(DirectBytestringTest, OversizedDeltaBatchRejectedAsConflict)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000'000);

  // Positive control: a normal small batch commits.
  {
    auto tx = make_new_per_tx_db_proxy();
    for (int i = 0; i < 10; ++i) {
      tx.add_delta(make_addrkey(contract_addr, "k" + std::to_string(i)),
                   make_bytestring_delta("value"),
                   std::to_underlying(StorageDeltaFlags::None));
    }
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r.value()) << "a small delta batch must commit";
  }

  // A batch of max-size (1 KiB) bytestrings across enough distinct keys to
  // exceed the 16 MiB cap. Each delta is ~1104 bytes (key+framing+1028), so
  // 16000 deltas ~= 16.8 MiB > cap. Rejected before any push is attempted.
  {
    const std::string max_value(1024, 'x'); // kBytestringMaxLen
    auto tx = make_new_per_tx_db_proxy();
    for (int i = 0; i < 16000; ++i) {
      tx.add_delta(make_addrkey(contract_addr, "spray" + std::to_string(i)),
                   make_bytestring_delta(max_value),
                   std::to_underlying(StorageDeltaFlags::None));
    }
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value())
        << "over-budget push must be a clean conflict, not a system error";
    EXPECT_FALSE(r.value())
        << "an over-cap delta batch must be rejected as a conflict";
  }
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, DirectBytestringTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

} // namespace hogd
