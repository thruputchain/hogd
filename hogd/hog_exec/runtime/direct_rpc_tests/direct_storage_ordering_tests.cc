#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "contract.hh"
#include "db_conflict.hh"
#include "executor_to_db_rpc.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/borrowed_contract_batch.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/rpc_common.h"

namespace hogd {

// NOLINTBEGIN(bugprone-unchecked-optional-access)
// NOLINTNEXTLINE(misc-use-internal-linkage)
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

  constexpr static BorrowedContractBatch empty_contract_batch()
  {
    static const BorrowedContractBatch::hash_set_t kEmptyHashes;
    static const BorrowedContractBatch::contract_map_t kEmptyContracts;
    return BorrowedContractBatch(kEmptyHashes, kEmptyContracts);
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

  auto r =
      db_proxy.try_push_new_subtransaction(get_block_number(), 1, deltas)();
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(r.value());
  ASSERT_TRUE(
      db_proxy
          .commit_transaction(get_block_number(), 1, empty_contract_batch())()
          .has_value());
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

  auto r = proxy.try_push_current_subtransaction()();
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

  auto r2 = proxy.try_push_current_subtransaction()();
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2.value());
  ASSERT_TRUE(proxy.commit_transaction()().has_value());

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

  auto r =
      db_proxy.try_push_new_subtransaction(get_block_number(), 1, deltas)();
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(r.value());
  ASSERT_TRUE(
      db_proxy
          .commit_transaction(get_block_number(), 1, empty_contract_batch())()
          .has_value());
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

TEST_P(DirectStorageOrderingTest, DiagnosticOnConflict)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not produce conflict diagnostics";
  }

  auto contract_addr0 = make_contract_address(0);
  setup_grant_storage_token(contract_addr0, 2 * cost_model_->get_nnint_cost());

  DatabaseProxy &db_proxy = get_db_proxy();
  auto key = make_contract_data_addrkey(contract_addr0, "conflict");

  StorageDeltaBatch winner;
  winner.push_back(FlaggedStorageDelta{.addrkey = key,
                                       .delta = make_nnint_set_add_delta(0, 1),
                                       .flags = std::to_underlying(
                                           StorageDeltaFlags::None)});
  auto r1 =
      db_proxy.try_push_new_subtransaction(get_block_number(), 1, winner)();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  // Different base value at the same key from another tx: value mismatch.
  StorageDeltaBatch loser;
  loser.push_back(FlaggedStorageDelta{.addrkey = key,
                                      .delta = make_nnint_set_add_delta(5, 1),
                                      .flags = std::to_underlying(
                                          StorageDeltaFlags::None)});
  DbConflictResult conflict(DbDiagnosticLevel::kDbDiagnosticNone);
  auto r2 = db_proxy.try_push_new_subtransaction(get_block_number(), 2, loser,
                                                 &conflict)();
  ASSERT_TRUE(r2.has_value());
  ASSERT_FALSE(r2.value());
  // The fixture runs Proposal mode: hog_db selects All in debug builds,
  // First in release builds.
  if constexpr (is_debug_build()) {
    ASSERT_EQ(conflict.level(), DbDiagnosticLevel::kDbDiagnosticAll);
    ASSERT_EQ(conflict.conflicts().size(), 1U);
    EXPECT_EQ(conflict.conflicts()[0].addrkey, key);
    EXPECT_EQ(conflict.conflicts()[0].code,
              DbConflictCode::kDbConflictValueMismatch);
  }
  else {
    ASSERT_EQ(conflict.level(), DbDiagnosticLevel::kDbDiagnosticFirst);
    EXPECT_EQ(conflict.first().addrkey, key);
    EXPECT_EQ(conflict.first().code, DbConflictCode::kDbConflictValueMismatch);
  }
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(AllDBs, DirectStorageOrderingTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

// Regression for #1975: an oversized StorageDeltaBatch used to throw
// x23::XdrBadInput out of
// PerTransactionDatabaseProxy::try_push_current_subtransaction(), which both
// aborted detached executor workers and skipped the thread-local batch clear,
// contaminating the next transaction on the same thread. The fix wraps the
// inner client call to convert the exception into
// SystemErrorCode::Unrecoverable, which also lets the post-call clear run.
TEST(DirectStorageOversizedDeltaBatchTest, OversizedBatchYieldsUnrecoverable)
{
  // Shrink the SRPC record cap before constructing the framework so both the
  // exec-side client and the DB-side server adopt it.
  const std::size_t saved_max = x23::default_max_message_size;
  x23::default_max_message_size = 1024;
  x23::Defer restore{
      [saved_max] { x23::default_max_message_size = saved_max; }};

  NativeContractTestFramework fw(
      /*log=*/false, BlockExecutionMode::kBlockExecutionModeProposal,
      TestDB::kHogDb, /*empty_genesis=*/false);
  fw.run_genesis_initialization(
      NativeContractTestFramework::make_compact_test_genesis_configuration());

  const auto contract_addr =
      NativeContractTestFramework::make_contract_address(0);
  fw.setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto oversized_proxy = fw.make_new_per_tx_db_proxy(101);
  for (std::size_t i = 0; i < 32; ++i) {
    oversized_proxy.add_delta(
        NativeContractTestFramework::make_contract_data_addrkey(
            contract_addr, "poc_" + std::to_string(i)),
        NativeContractTestFramework::make_bytestring_delta(
            std::string(64, 'x')),
        std::to_underlying(StorageDeltaFlags::None));
  }

  // Must not throw — the wrapper converts x23::XdrBadInput to Unrecoverable.
  auto r = oversized_proxy.try_push_current_subtransaction()();
  ASSERT_FALSE(r.has_value());
  ASSERT_EQ(r.error().code(), SystemErrorCode::Unrecoverable);
}

TEST(DirectStorageOversizedFinalCommitBatchTest,
     OversizedNewContractBatchYieldsUnrecoverable)
{
  const std::size_t saved_max = x23::default_max_message_size;
  x23::default_max_message_size = 1024;
  x23::Defer restore{
      [saved_max] { x23::default_max_message_size = saved_max; }};

  NativeContractTestFramework fw(
      /*log=*/false, BlockExecutionMode::kBlockExecutionModeProposal,
      TestDB::kHogDb, /*empty_genesis=*/false);
  fw.run_genesis_initialization(
      NativeContractTestFramework::make_compact_test_genesis_configuration());

  const auto contract_addr =
      NativeContractTestFramework::make_contract_address(1);
  fw.setup_grant_storage_token(contract_addr, 1'000'000'000);

  Contract contract(ContractType::kRv32);
  contract.rv32_contract().data.append_range(std::string(2048, 'x'));
  auto contract_ptr = std::make_shared<const Contract>(contract);
  const auto deployed_contract_hash =
      NativeContractTestFramework::make_deployed_contract_hash(contract_ptr);

  auto tx = fw.make_new_per_tx_db_proxy(202);
  AddressAndKey addrkey{.address = contract_addr,
                        .key = StorageKey(StorageKeyType::Metadata)};
  StorageDelta delta(StorageDeltaType::kDeltaTypeDeployContract);
  delta.deployed_contract_hash() = deployed_contract_hash;
  tx.add_delta(addrkey, std::move(delta),
               std::to_underlying(StorageDeltaFlags::None));

  auto push = tx.try_push_current_subtransaction()();
  ASSERT_TRUE(push.has_value());
  ASSERT_TRUE(push.value());

  BorrowedContractBatch::hash_set_t hashes{deployed_contract_hash.hash};
  BorrowedContractBatch::contract_map_t contracts;
  contracts.emplace(deployed_contract_hash.hash,
                    std::make_unique<const Contract>(contract));
  BorrowedContractBatch new_contracts(hashes, contracts);

  auto res = tx.commit_transaction(new_contracts)();
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), SystemErrorCode::Unrecoverable);
}

} // namespace hogd
