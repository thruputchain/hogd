#include <cstdint>
#include <gtest/gtest.h>

#include <utility>

#include "executor_to_db_rpc.hh"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

// NOLINTBEGIN(bugprone-unchecked-optional-access)

class DirectStorageOrderingTest : public ::testing::TestWithParam<TestDB>,
                                  public NativeContractTestFramework {
public:
  DirectStorageOrderingTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

  void SetUp() override { cost_model_ = get_default_cost_model(); }

  const StorageCostModel *cost_model_;

  PerTransactionDatabaseProxy make_new_per_tx_db_proxy()
  {
    return NativeContractTestFramework::make_new_per_tx_db_proxy(UINT64_MAX);
  }
};

TEST_P(DirectStorageOrderingTest, TransferOrdering)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP()
        << "MockDB is broken when having manual transfers + storage costs";
  }

  auto contract_addr0 = make_contract_address(0);
  auto contract_addr1 = make_contract_address(1);

  setup_grant_storage_token(contract_addr0, cost_model_->get_nnint_cost());
  setup_grant_storage_token(contract_addr1, 0);

  DatabaseProxy &db_proxy = get_db_proxy();

  StorageDeltaBatch deltas;

  // transfer units from contract 0 (to contract 1)
  deltas.push_back(FlaggedStorageDelta{
      .addrkey = make_free_native_token_balance_addrkey(contract_addr0),
      .delta = make_nnint_add_delta(-cost_model_->get_nnint_cost()),
      .flags = std::to_underlying(StorageDeltaFlags::None)});
  // use the units received in the next delta to write a storage item
  deltas.push_back(FlaggedStorageDelta{
      .addrkey = make_contract_data_addrkey(contract_addr1, "test"),
      .delta = make_nnint_set_add_delta(0, 1),
      .flags = std::to_underlying(StorageDeltaFlags::None)});
  // receive the units in contract 1 (from contract 0)
  deltas.push_back(FlaggedStorageDelta{
      .addrkey = make_free_native_token_balance_addrkey(contract_addr1),
      .delta = make_nnint_add_delta(cost_model_->get_nnint_cost()),
      .flags = std::to_underlying(StorageDeltaFlags::None)});

  auto r = db_proxy.try_push_new_subtransaction(get_block_number(), 1, deltas);
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(r.value());
  ASSERT_TRUE(db_proxy.commit_transaction(get_block_number(), 1).has_value());
  advance_block();

  auto res = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_addr1, "test"));
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res->type(), StorageItemType::kItemTypeNonnegativeInt);
  ASSERT_EQ(from_xdr(res->nnint()), 1);

  auto contract_bal0 = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr0));
  ASSERT_TRUE(contract_bal0.has_value());
  ASSERT_EQ(from_xdr(contract_bal0.value().nnint()), 0);

  auto contract_bal1 = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr1));
  ASSERT_TRUE(contract_bal1.has_value());
  ASSERT_EQ(from_xdr(contract_bal1.value().nnint()), 0);
}

TEST_P(DirectStorageOrderingTest, TransferWithTryInvoke)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB is broken when having multiple overwriting balance "
                    "transfers + storage costs";
  }

  auto contract_addr0 = make_contract_address(0);
  auto contract_addr1 = make_contract_address(1);

  setup_grant_storage_token(contract_addr0, cost_model_->get_nnint_cost());
  setup_grant_storage_token(contract_addr1, 0);

  auto proxy = make_new_per_tx_db_proxy();
  proxy.add_delta(make_free_native_token_balance_addrkey(contract_addr0),
                  make_nnint_add_delta(-cost_model_->get_nnint_cost()),
                  std::to_underlying(StorageDeltaFlags::None));
  proxy.add_delta(make_free_native_token_balance_addrkey(contract_addr1),
                  make_nnint_add_delta(cost_model_->get_nnint_cost()),
                  std::to_underlying(StorageDeltaFlags::None));

  auto r = proxy.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(r.value());

  proxy.add_delta(make_contract_data_addrkey(contract_addr0, "test"),
                  make_nnint_set_add_delta(0, 1),
                  std::to_underlying(StorageDeltaFlags::None));

  // Undo the transfer
  proxy.add_delta(make_free_native_token_balance_addrkey(contract_addr0),
                  make_nnint_add_delta(0),
                  std::to_underlying(StorageDeltaFlags::None));
  proxy.add_delta(make_free_native_token_balance_addrkey(contract_addr1),
                  make_nnint_add_delta(0),
                  std::to_underlying(StorageDeltaFlags::None));

  auto r2 = proxy.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());
  ASSERT_TRUE(proxy.commit_transaction().has_value());

  advance_block();

  auto res = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_addr0, "test"));
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res->type(), StorageItemType::kItemTypeNonnegativeInt);
  ASSERT_EQ(from_xdr(res->nnint()), 1);

  auto contract_bal0 = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr0));
  ASSERT_TRUE(contract_bal0.has_value());
  ASSERT_EQ(from_xdr(contract_bal0.value().nnint()), 0);

  auto contract_bal1 = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr1));
  ASSERT_TRUE(contract_bal1.has_value());
  ASSERT_EQ(from_xdr(contract_bal1.value().nnint()), 0);
}

TEST_P(DirectStorageOrderingTest, TransferOrderingWithNewBalanceKey)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP()
        << "MockDB is broken when having manual transfers + storage costs";
  }

  auto contract_addr0 = make_contract_address(0);
  auto contract_addr1 = make_contract_address(1);

  setup_grant_storage_token(contract_addr0,
                            cost_model_->get_nnint_cost() +
                                cost_model_->get_balance_cost());

  DatabaseProxy &db_proxy = get_db_proxy();

  StorageDeltaBatch deltas;

  // transfer units from contract 0 (to contract 1)
  deltas.push_back(FlaggedStorageDelta{
      .addrkey = make_free_native_token_balance_addrkey(contract_addr0),
      .delta = make_nnint_add_delta(-cost_model_->get_nnint_cost() -
                                    cost_model_->get_balance_cost()),
      .flags = std::to_underlying(StorageDeltaFlags::None)});
  // use the units received in the next delta to write a storage item
  deltas.push_back(FlaggedStorageDelta{
      .addrkey = make_contract_data_addrkey(contract_addr1, "test"),
      .delta = make_nnint_set_add_delta(0, 1),
      .flags = std::to_underlying(StorageDeltaFlags::None)});
  // receive the units in contract 1 (from contract 0)
  deltas.push_back(FlaggedStorageDelta{
      .addrkey = make_free_native_token_balance_addrkey(contract_addr1),
      .delta = make_nnint_add_delta(cost_model_->get_nnint_cost() +
                                    cost_model_->get_balance_cost()),
      .flags = std::to_underlying(StorageDeltaFlags::None)});

  auto r = db_proxy.try_push_new_subtransaction(get_block_number(), 1, deltas);
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(r.value());
  ASSERT_TRUE(db_proxy.commit_transaction(get_block_number(), 1).has_value());
  advance_block();

  auto res = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_addr1, "test"));
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res->type(), StorageItemType::kItemTypeNonnegativeInt);
  ASSERT_EQ(from_xdr(res->nnint()), 1);

  auto contract_bal0 = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr0));
  ASSERT_TRUE(contract_bal0.has_value());
  ASSERT_EQ(from_xdr(contract_bal0.value().nnint()), 0);

  auto contract_bal1 = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr1));
  ASSERT_TRUE(contract_bal1.has_value());
  ASSERT_EQ(from_xdr(contract_bal1.value().nnint()), 0);
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(AllDBs, DirectStorageOrderingTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
