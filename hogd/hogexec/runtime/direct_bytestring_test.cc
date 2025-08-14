#include <gtest/gtest.h>

#include <string_view>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

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
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

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
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP() << "Pending hogdb cost sign fix";
    return;
  }
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

  ASSERT_TRUE(tx1.commit_transaction().has_value());
  ASSERT_TRUE(tx1_1.commit_transaction().has_value());
  ASSERT_TRUE(tx1_2.commit_transaction().has_value());
  ASSERT_TRUE(tx2.commit_transaction().has_value());

  advance_block();
  // check that costs are reflected in the balance
  auto bal1 = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr1));
  auto bal2 = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr2));
  ASSERT_TRUE(bal1.has_value());
  ASSERT_TRUE(bal2.has_value());
  int128_t bal1_int = from_xdr(bal1.value().nnint());
  int128_t bal2_int = from_xdr(bal2.value().nnint());

  EXPECT_EQ(bal1_int, bal2_int);
  EXPECT_LT(bal1_int, kInitialBalance);
}

TEST_P(DirectBytestringTest, RevertStorageCosts)
{
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP() << "Pending hogdb cost sign fix";
    return;
  }
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
  ASSERT_TRUE(tx2.commit_transaction().has_value());

  // compare with single tx address
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_addrkey(reference_addr, "test"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r3 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value());
  ASSERT_TRUE(tx3.commit_transaction().has_value());

  advance_block();

  auto bal1 = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(bal1.has_value());
  EXPECT_LT(from_xdr(bal1.value().nnint()), 1'000'000'000);

  auto bal2 = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(reference_addr));
  ASSERT_TRUE(bal2.has_value());
  EXPECT_EQ(from_xdr(bal2.value().nnint()), from_xdr(bal1.value().nnint()));
}

TEST_P(DirectBytestringTest, LengthIncreaseCost)
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

  auto tx_init = make_new_per_tx_db_proxy();
  tx_init.add_delta(make_addrkey(contract_addr, "test"),
                    make_bytestring_delta("value"),
                    std::to_underlying(StorageItemRPCFlags::kStorageNone));
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
  ASSERT_TRUE(tx2.commit_transaction().has_value());
  ASSERT_TRUE(tx3.abort_transaction().has_value());

  // reference address
  auto tx_ref = make_new_per_tx_db_proxy();
  tx_ref.add_delta(make_addrkey(reference_addr, "test"),
                   make_bytestring_delta("much_longer_value"),
                   std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_ref = tx_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_ref.has_value());
  ASSERT_TRUE(r_ref.value());
  ASSERT_TRUE(tx_ref.commit_transaction().has_value());

  advance_block();

  auto bal1 = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(bal1.has_value());
  EXPECT_LT(from_xdr(bal1.value().nnint()), kInitialBalance);

  auto bal2 = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(reference_addr));
  ASSERT_TRUE(bal2.has_value());
  EXPECT_EQ(from_xdr(bal2.value().nnint()), from_xdr(bal1.value().nnint()));
}

TEST_P(DirectBytestringTest, DecreaseLengthCost)
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

  auto tx_init = make_new_per_tx_db_proxy();
  tx_init.add_delta(make_addrkey(contract_addr, "test"),
                    make_bytestring_delta("long_initial_value"),
                    std::to_underlying(StorageItemRPCFlags::kStorageNone));
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
                 std::to_underlying(StorageItemRPCFlags::kStorageNone));
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
                   std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r_ref = tx_ref.try_push_current_subtransaction();
  ASSERT_TRUE(r_ref.has_value());
  ASSERT_TRUE(r_ref.value());
  ASSERT_TRUE(tx_ref.commit_transaction().has_value());

  advance_block();

  auto bal1 = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(bal1.has_value());
  EXPECT_LT(from_xdr(bal1.value().nnint()), kInitialBalance);

  auto bal2 = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(reference_addr));
  ASSERT_TRUE(bal2.has_value());
  EXPECT_EQ(from_xdr(bal2.value().nnint()), from_xdr(bal1.value().nnint()));
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
  ASSERT_TRUE(tx_init.commit_transaction().has_value());
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
  ASSERT_TRUE(tx2.commit_transaction().has_value());
  ASSERT_TRUE(tx3.commit_transaction().has_value());

  advance_block();

  auto bal1 = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));

  ASSERT_TRUE(bal1.has_value());
  EXPECT_EQ(from_xdr(bal1.value().nnint()), kInitialBalance);
}
// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, DirectBytestringTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
