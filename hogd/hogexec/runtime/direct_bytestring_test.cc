#include <gtest/gtest.h>

#include <string_view>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "rpc_types.hh"
#include "storage_delta_flags.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

// NOLINTBEGIN(bugprone-unchecked-optional-access)

class DirectBytestringTest : public ::testing::TestWithParam<TestDB>,
                             public NativeContractTestFramework {
public:
  DirectBytestringTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

  void SetUp() override {}

  static auto make_addrkey(ContractAddress contract_addr, std::string_view key)
  {
    return make_contract_data_addrkey(contract_addr, key);
  }

  static auto make_bytestring_delta(std::string_view bstring)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeBytestringSet);
    delta.bytestring() = make_bytestring(bstring);
    return delta;
  }

  static auto make_delete_delta()
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeDelete);
    return delta;
  }

  // check that the two addresses have the same balance,
  // and that that balance is less than or equal to
  // the initial balance
  void check_balances(ContractAddress contract_addr,
                      ContractAddress reference_addr, int128_t initial_balance,
                      bool eq_ok = false)
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
    if (eq_ok) {
      EXPECT_LE(bal_int, initial_balance);
    }
    else {
      EXPECT_LT(bal_int, initial_balance);
    }
  }
};

TEST_P(DirectBytestringTest, ConcurrentWrites)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // 3 identical writes ok, different write bad
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  auto tx_diff = make_new_per_tx_db_proxy();
  tx_diff.add_delta(make_addrkey(contract_addr, "test"),
                    make_bytestring_delta("different_value"),
                    std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_diff = tx_diff.try_push_current_subtransaction();
  ASSERT_TRUE(r_diff.has_value());
  ASSERT_FALSE(r_diff.value());

  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
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
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  auto tx1_1 = make_new_per_tx_db_proxy();
  tx1_1.add_delta(make_addrkey(contract_addr1, "test"),
                  make_bytestring_delta("value"),
                  std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1_1 = tx1_1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_1.has_value());
  ASSERT_TRUE(r1_1.value());

  auto tx1_2 = make_new_per_tx_db_proxy();
  tx1_2.add_delta(make_addrkey(contract_addr1, "test"),
                  make_bytestring_delta("value"),
                  std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1_2 = tx1_2.try_push_current_subtransaction();
  ASSERT_TRUE(r1_2.has_value());
  ASSERT_TRUE(r1_2.value());

  // create the same bytestring on a different contract
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr2, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx1_1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx1_2.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());

  advance_block();
  check_balances(contract_addr1, contract_addr2, kInitialBalance);
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
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  ASSERT_TRUE(tx1.abort_transaction().has_value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());

  // compare with single tx address
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_addrkey(reference_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r3 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value());
  ASSERT_TRUE(tx3.commit_transaction({}).has_value());

  advance_block();

  check_balances(contract_addr, reference_addr, kInitialBalance);
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
                    std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_init = tx_init.try_push_current_subtransaction();
  ASSERT_TRUE(r_init.has_value());
  ASSERT_TRUE(r_init.value());

  ASSERT_TRUE(tx_init.commit_transaction({}).has_value());
  advance_block();

  // Test begins:
  // Create a bytestring of longer length, which costs more.
  // Three txs create this string, but the first and the third revert.

  auto make_test_call = [&]() {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_addrkey(contract_addr, "test"),
                 make_bytestring_delta("much_longer_value"),
                 std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx.try_push_current_subtransaction();
    EXPECT_TRUE(r.has_value());
    EXPECT_TRUE(r.value());
    return tx;
  };

  auto tx1 = make_test_call();
  auto tx2 = make_test_call();
  auto tx3 = make_test_call();
  ASSERT_TRUE(tx1.abort_transaction().has_value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  ASSERT_TRUE(tx3.abort_transaction().has_value());

  // reference address
  auto tx_ref = make_new_per_tx_db_proxy();
  tx_ref.add_delta(make_addrkey(reference_addr, "test"),
                   make_bytestring_delta("much_longer_value"),
                   std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_ref = tx_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_ref.has_value());
  ASSERT_TRUE(r_ref.value());
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();
  check_balances(contract_addr, reference_addr, kInitialBalance);
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
                    std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_init = tx_init.try_push_current_subtransaction();
  ASSERT_TRUE(r_init.has_value());
  ASSERT_TRUE(r_init.value());
  ASSERT_TRUE(tx_init.commit_transaction({}).has_value());
  advance_block();

  // Test begins:
  // Create a bytestring of shorter length, which costs less.
  // Three txs create this string, but the first and the third revert.

  auto make_test_call = [&]() {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_addrkey(contract_addr, "test"),
                 make_bytestring_delta("v"), // short value
                 std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx.try_push_current_subtransaction();
    EXPECT_TRUE(r.has_value());
    return tx;
  };

  auto tx1 = make_test_call();
  auto tx2 = make_test_call();
  auto tx3 = make_test_call();
  ASSERT_TRUE(tx1.abort_transaction().has_value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  ASSERT_TRUE(tx3.abort_transaction().has_value());

  // reference address
  auto tx_ref = make_new_per_tx_db_proxy();
  tx_ref.add_delta(make_addrkey(reference_addr, "test"),
                   make_bytestring_delta("v"),
                   std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_ref = tx_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_ref.has_value());
  ASSERT_TRUE(r_ref.value());
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();
  check_balances(contract_addr, reference_addr, kInitialBalance);
}

TEST_P(DirectBytestringTest, DeletionCost)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto tx_init = make_new_per_tx_db_proxy();
  tx_init.add_delta(make_addrkey(contract_addr, "test"),
                    make_bytestring_delta("value"),
                    std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_init = tx_init.try_push_current_subtransaction();
  ASSERT_TRUE(r_init.has_value());
  ASSERT_TRUE(r_init.value());
  ASSERT_TRUE(tx_init.commit_transaction({}).has_value());
  advance_block();

  // Test begins:
  // Delete the bytestring
  auto make_test_call = [&]() {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                 std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx.try_push_current_subtransaction();
    EXPECT_TRUE(r.has_value());
    return tx;
  };

  auto tx1 = make_test_call();
  auto tx2 = make_test_call();
  auto tx3 = make_test_call();
  ASSERT_TRUE(tx1.abort_transaction().has_value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  ASSERT_TRUE(tx3.commit_transaction({}).has_value());

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
                    std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_init = tx_init.try_push_current_subtransaction();
  ASSERT_TRUE(r_init.has_value());
  ASSERT_TRUE(r_init.value());
  ASSERT_TRUE(tx_init.commit_transaction({}).has_value());
  advance_block();

  // Start the test

  // tx1 deletes the key
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  // tx2 deletes the key
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  // tx1 deletes the key again
  tx1.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1_1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_1.has_value());
  ASSERT_TRUE(r1_1.value());

  // tx2 aborts
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  // tx1 commits
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

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
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_longer"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_even_longer"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r3 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value());

  ASSERT_TRUE(tx1.pop_subtransactions(1).has_value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  auto tx_ref = make_new_per_tx_db_proxy();
  tx_ref.add_delta(make_addrkey(reference_addr, "test"),
                   make_bytestring_delta("value_longer"),
                   std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_ref = tx_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_ref.has_value());
  ASSERT_TRUE(r_ref.value());
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();
  check_balances(contract_addr, reference_addr, kInitialBalance);
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
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_longer"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r3 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value());

  ASSERT_TRUE(tx1.pop_subtransactions(1).has_value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  auto tx_ref = make_new_per_tx_db_proxy();
  tx_ref.add_delta(make_addrkey(reference_addr, "test"),
                   make_bytestring_delta("value_longer"),
                   std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_ref = tx_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_ref.has_value());
  ASSERT_TRUE(r_ref.value());
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();
  check_balances(contract_addr, reference_addr, kInitialBalance);
}

TEST_P(DirectBytestringTest, TryInvokeReleaseExclusiveLock)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_longer"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1_1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_1.has_value());
  ASSERT_TRUE(r1_1.value());

  // now tx1 holds the exclusive lock

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_longer"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_FALSE(r2.value()); // should fail because tx1 holds the exclusive lock

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // now the exclusive lock is no longer held
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_longer"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2_1 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_1.has_value());
  ASSERT_TRUE(r2_1.value());

  // but tx2 cannot write a different value
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_even_longer"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2_2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2_2.has_value());
  ASSERT_FALSE(r2_2.value());

  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
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
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP() << "Pending hogdb cost sign fix";
    return;
  }
  auto contract_addr = make_contract_address(0);
  auto reference_addr = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);
  setup_grant_storage_token(reference_addr, kInitialBalance);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_longer"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1_1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_1.has_value());
  ASSERT_TRUE(r1_1.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_longer"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  ASSERT_TRUE(tx2.commit_transaction({}).has_value());

  auto tx_ref = make_new_per_tx_db_proxy();
  tx_ref.add_delta(make_addrkey(reference_addr, "test"),
                   make_bytestring_delta("value_longer"),
                   std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_ref = tx_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_ref.has_value());
  ASSERT_TRUE(r_ref.value());
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();
  check_balances(contract_addr, reference_addr, kInitialBalance);
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
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_longer"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1_1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_1.has_value());
  ASSERT_FALSE(r1_1.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  // reference: the first bytestring write is what commits
  auto tx_ref = make_new_per_tx_db_proxy();
  tx_ref.add_delta(make_addrkey(reference_addr, "test"),
                   make_bytestring_delta("value"),
                   std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_ref = tx_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_ref.has_value());
  ASSERT_TRUE(r_ref.value());
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();
  check_balances(contract_addr, reference_addr, kInitialBalance);
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
                    std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_init = tx_init.try_push_current_subtransaction();
  ASSERT_TRUE(r_init.has_value());
  ASSERT_TRUE(r_init.value());
  ASSERT_TRUE(tx_init.commit_transaction({}).has_value());

  auto tx_init_ref = make_new_per_tx_db_proxy();
  tx_init_ref.add_delta(make_addrkey(reference_addr, "test_new"),
                        make_bytestring_delta("value"),
                        std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_init_ref = tx_init_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_init_ref.has_value());
  ASSERT_TRUE(r_init_ref.value());
  ASSERT_TRUE(tx_init_ref.commit_transaction({}).has_value());
  advance_block();

  // start the test

  // tx2: just delete the key
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  // tx1: Creates the new key, then deletes the old one, in one operation
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test_new"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  tx1.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  advance_block();
  check_balances(contract_addr, reference_addr, kInitialBalance);
}

TEST_P(DirectBytestringTest, DeletionCostNotImmediatelyAvailable)
{
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP() << "Assertion failed: (base_ + minus_ >= 0), function "
                    "assert_is_valid, file item_change.h, line 103.";
    return;
  }
  auto contract_addr = make_contract_address(0);
  auto reference_addr = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(reference_addr, kInitialBalance);

  auto tx_ref_init = make_new_per_tx_db_proxy();
  tx_ref_init.add_delta(make_addrkey(reference_addr, "test_new"),
                        make_bytestring_delta("value"),
                        std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_init = tx_ref_init.try_push_current_subtransaction();
  ASSERT_TRUE(r_init.has_value());
  ASSERT_TRUE(r_init.value());
  ASSERT_TRUE(tx_ref_init.commit_transaction({}).has_value());
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
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  advance_block();
  auto initial_bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(initial_bal.has_value());
  int128_t initial_bal_int = from_xdr(initial_bal.value().nnint());
  EXPECT_EQ(initial_bal_int, 0);

  // Now the test begins.

  // This test has two txs:
  // The first deletes the key, which should free up the storage cost.
  // However, that cost release cannot occur util the tx commits;
  // hence, the call to tx3.try_push fails the first time,
  // and succeeds the second time (after tx2.commit)

  // tx2: just delete the key
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  // tx3: create a new key
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_addrkey(contract_addr, "test_new"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r3 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_FALSE(r3.value()); // storage not yet available

  ASSERT_TRUE(tx2.commit_transaction({}).has_value());

  // now storage is available
  tx3.add_delta(make_addrkey(contract_addr, "test_new"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r3_1 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3_1.has_value());
  ASSERT_TRUE(r3_1.value());

  ASSERT_TRUE(tx3.commit_transaction({}).has_value());

  advance_block();
  auto bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(bal.has_value());
  int128_t bal_int = from_xdr(bal.value().nnint());
  EXPECT_EQ(bal_int, 0);
}

TEST_P(DirectBytestringTest, RollbackPartwayThroughSavepoint)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto tx_block = make_new_per_tx_db_proxy();
  tx_block.add_delta(make_addrkey(contract_addr, "test"),
                     make_bytestring_delta("value"),
                     std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_block = tx_block.try_push_current_subtransaction();
  ASSERT_TRUE(r_block.has_value());
  ASSERT_TRUE(r_block.value());
  ASSERT_TRUE(tx_block.commit_transaction({}).has_value());

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test_nonblock"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("value_different"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_FALSE(r1.value());

  ASSERT_TRUE(tx1.abort_transaction().has_value());
}

TEST_P(DirectBytestringTest, DeleteThenSetConflict)
{
  // This test has two txs.
  // First two txs delete a key.  But before committing,
  // the first tx tries to set a new value (which fails).
  // The first tx commits (the deletion) and the second aborts.

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
                      std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r_init = tx_init.try_push_current_subtransaction();
    ASSERT_TRUE(r_init.has_value());
    ASSERT_TRUE(r_init.value());
    ASSERT_TRUE(tx_init.commit_transaction({}).has_value());

    // initialize the reference address

    auto tx_init_ref = make_new_per_tx_db_proxy();
    tx_init_ref.add_delta(make_addrkey(reference_addr, "test"),
                          make_bytestring_delta("value"),
                          std::to_underlying(
                              StorageItemRPCFlags::kStorageNone));
    auto r_init_ref = tx_init_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r_init_ref.has_value());
    ASSERT_TRUE(r_init_ref.value());
    ASSERT_TRUE(tx_init_ref.commit_transaction({}).has_value());

    advance_block();
  }

  // Start conflict scenario: two deletes, then one tries to set
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_addrkey(contract_addr, "test"), make_delete_delta(),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());

  // Attempt to set a new value in tx1 while tx2 still holds a lock
  tx1.add_delta(make_addrkey(contract_addr, "test"),
                make_bytestring_delta("new_value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1_set = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1_set.has_value());
  ASSERT_FALSE(r1_set.value()); // cannot upgrade under shared lock in mockdb

  ASSERT_TRUE(tx1.commit_transaction({}).has_value()); // deletes the key
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  // Reference: just a single delete on another address
  auto tx_ref = make_new_per_tx_db_proxy();
  tx_ref.add_delta(make_addrkey(reference_addr, "test"), make_delete_delta(),
                   std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_ref = tx_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_ref.has_value());
  // If key absent, delete is treated as no-op but allowed in mockdb path
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();
  check_balances(contract_addr, reference_addr, kInitialBalance,
                 /*eq_ok=*/true);
}

TEST_P(DirectBytestringTest, DeleteMissingKeyNoOp)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  // No prior write to this key; deletion should be a no-op.
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(make_addrkey(contract_addr, "missing_key"), make_delete_delta(),
               std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(r.value());
  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  advance_block();

  auto bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(bal.has_value());
  EXPECT_EQ(from_xdr(bal.value().nnint()), kInitialBalance);
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, DirectBytestringTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
