#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <utility>

#include "contract.hh"
#include "db_sched.hh"
#include "ffi_enums.hh"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/borrowed_contract_batch.h"
#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

class DirectStalenessTest : public ::testing::Test,
                            public NativeContractTestFramework {
public:
  DirectStalenessTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

  struct SiblingBlocks {
    BlockID block_a;
    BlockID block_b;
  };

  SiblingBlocks create_sibling()
  {
    BlockID block_a = get_block_number();
    BlockID parent_id = get_db_tip().id;

    constexpr BlockID kSiblingOffset = 1000;
    BlockID block_b = block_a + kSiblingOffset;
    auto r = get_coord_client().dbsched_create_snapshot(
        parent_id, block_b, BlockExecutionMode::kBlockExecutionModeProposal);
    EXPECT_EQ(r, DbSchedResult::DbSchedResultOK);

    return {.block_a = block_a, .block_b = block_b};
  }

  void close_and_commit_block(BlockID block)
  {
    auto close = get_coord_client().dbsched_close_snapshot(block);
    EXPECT_EQ(close.result(), DbSchedResult::DbSchedResultOK);

    auto commit = get_coord_client().dbsched_commit_snapshot(
        block,
        // NOLINTNEXTLINE(bugprone-argument-comment)
        /*block_hash=*/BlockHash{}, /*events_hash=*/SHA256Hash{});
    EXPECT_EQ(commit.result(), DbSchedResult::DbSchedResultOK);
  }

  SiblingBlocks create_and_commit_sibling()
  {
    auto blocks = create_sibling();
    close_and_commit_block(blocks.block_a);
    return blocks;
  }
};

TEST_F(DirectStalenessTest, SnapshotItemReadOnStaleBlockReturnsAbandon)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto key = make_contract_data_addrkey(contract_addr, "test_key");

  {
    auto tx = make_new_per_tx_db_proxy(0);
    tx.add_delta(key, make_nnint_set_add_delta(0, 42),
                 std::to_underlying(StorageDeltaFlags::None));
    ASSERT_TRUE(tx.try_push_current_subtransaction().has_value());
    ASSERT_TRUE(tx.commit_transaction().has_value());
  }

  advance_block();

  auto [block_a, block_b] = create_and_commit_sibling();

  auto res = get_db_proxy().get_snapshot_item(block_b, key);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), SystemErrorCode::Abandon);
}

TEST_F(DirectStalenessTest, ContractReadOnStaleBlockReturnsAbandon)
{
  auto contract = make_native_contract(0);
  write_contract(contract);

  advance_block();

  auto [block_a, block_b] = create_and_commit_sibling();

  auto res = get_db_proxy().get_contract(block_b, sha256(*contract));
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), SystemErrorCode::Abandon);
}

TEST_F(DirectStalenessTest, DeploymentReadOnStaleBlockReturnsAbandon)
{
  auto contract_addr = make_contract_address(1);
  auto contract = make_native_contract(0);
  write_deployment(contract_addr, contract);

  advance_block();

  auto [block_a, block_b] = create_and_commit_sibling();

  auto res = get_db_proxy().get_deployment(block_b, contract_addr);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), SystemErrorCode::Abandon);
}

TEST_F(DirectStalenessTest, DbTipReadOnStaleBlockReturnsAbandon)
{
  advance_block();

  auto [block_a, block_b] = create_and_commit_sibling();

  auto stale_snapshot_res = get_db_proxy().get_contract(block_b, SHA256Hash{});
  ASSERT_FALSE(stale_snapshot_res.has_value());
  EXPECT_EQ(stale_snapshot_res.error().code(), SystemErrorCode::Abandon);

  auto res = get_db_proxy().get_db_tip(block_b);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), SystemErrorCode::Abandon);
}

TEST_F(DirectStalenessTest,
       StaleNewContractCommitReturnsAbandonAndReleasesBlock)
{
  auto contract_addr = make_contract_address(2);
  setup_grant_storage_token(contract_addr, 1'000'000'000);
  advance_block();

  auto [block_a, block_b] = create_sibling();

  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract().wasm = {0x00, 0x61, 0x73, 0x6d,
                                    0x01, 0x00, 0x00, 0x00};
  auto contract_ptr = std::make_shared<const Contract>(contract);
  auto deployed_contract_hash = make_deployed_contract_hash(contract_ptr);

  constexpr TransactionID kTxId = 42;
  PerTransactionDatabaseProxy tx(get_db_proxy(), block_b, kTxId);
  AddressAndKey addrkey{.address = contract_addr,
                        .key = StorageKey(StorageKeyType::Metadata)};
  StorageDelta delta(StorageDeltaType::kDeltaTypeDeployContract);
  delta.deployed_contract_hash() = deployed_contract_hash;
  tx.add_delta(addrkey, std::move(delta),
               std::to_underlying(StorageDeltaFlags::None));
  auto push = tx.try_push_current_subtransaction();
  ASSERT_TRUE(push.has_value());
  ASSERT_TRUE(push.value());

  close_and_commit_block(block_a);

  BorrowedContractBatch::hash_set_t hashes{deployed_contract_hash.hash};
  BorrowedContractBatch::contract_map_t contracts;
  contracts.emplace(deployed_contract_hash.hash,
                    std::make_unique<const Contract>(contract));
  BorrowedContractBatch new_contracts(hashes, contracts);

  auto commit = tx.commit_transaction(new_contracts);
  ASSERT_FALSE(commit.has_value());
  EXPECT_EQ(commit.error().code(), SystemErrorCode::Abandon);

  // The invoke transaction reference should have been released, so the first
  // abandon actually removes the stale block and the second abandon fails to
  // find it.
  auto abandon = get_coord_client().dbsched_abandon_snapshot(block_b);
  EXPECT_EQ(abandon, DbSchedResult::DbSchedResultOK);
  auto second_abandon = get_coord_client().dbsched_abandon_snapshot(block_b);
  EXPECT_EQ(second_abandon, DbSchedResult::DbSchedResultError);
}

TEST_F(DirectStalenessTest, CommitInvalidatedBlockReturnsError)
{
  advance_block();

  auto [block_a, block_b] = create_sibling();

  close_and_commit_block(block_a);

  auto close_b = get_coord_client().dbsched_close_snapshot(block_b);
  EXPECT_EQ(close_b.result(), DbSchedResult::DbSchedResultOK);

  auto commit_b = get_coord_client().dbsched_commit_snapshot(
      block_b,
      // NOLINTNEXTLINE(bugprone-argument-comment)
      /*block_hash=*/BlockHash{}, /*events_hash=*/SHA256Hash{});
  EXPECT_NE(commit_b.result(), DbSchedResult::DbSchedResultOK);
}

// Viewing-snapshot refresh tests.
//
// These verify that opening a fresh viewing snapshot after a commit
// returns current data, even when a previous snapshot's
// StorageSnapshotCache was warm with stale entries.  This is the
// mechanism that ViewGatewayRpc::request_refresh() relies on.

TEST_F(DirectStalenessTest, FreshViewingSnapshotSeesUpdatedContractData)
{
  auto contract_addr = make_contract_address(10);
  setup_grant_storage_token(contract_addr, 1'000'000'000);
  auto addrkey = make_contract_data_addrkey(contract_addr, "view_key");

  {
    auto tx = make_new_per_tx_db_proxy(0);
    tx.add_delta(addrkey, make_bytestring_delta("old-value"),
                 std::to_underlying(StorageDeltaFlags::None));
    ASSERT_TRUE(tx.try_push_current_subtransaction().has_value());
    ASSERT_TRUE(tx.commit_transaction().has_value());
  }

  advance_block();

  auto snap1 = get_db_proxy().open_viewing_snapshot();
  ASSERT_TRUE(snap1.has_value());
  StorageSnapshotCache cache1(snap1->block_id, get_db_proxy());

  auto warm = cache1.try_get_snapshot_item(addrkey);
  ASSERT_TRUE(warm.has_value());
  ASSERT_NE(warm.value(), nullptr);
  EXPECT_EQ(warm.value()->bytestring(), make_bytestring("old-value"));

  {
    auto tx = make_new_per_tx_db_proxy(1);
    tx.add_delta(addrkey, make_bytestring_delta("new-value"),
                 std::to_underlying(StorageDeltaFlags::None));
    ASSERT_TRUE(tx.try_push_current_subtransaction().has_value());
    ASSERT_TRUE(tx.commit_transaction().has_value());
  }

  advance_block();

  // The stale cache still returns old-value.
  auto stale = cache1.try_get_snapshot_item(addrkey);
  ASSERT_TRUE(stale.has_value());
  ASSERT_NE(stale.value(), nullptr);
  EXPECT_EQ(stale.value()->bytestring(), make_bytestring("old-value"));

  // A fresh viewing snapshot sees new-value.
  auto snap2 = get_db_proxy().open_viewing_snapshot();
  ASSERT_TRUE(snap2.has_value());
  StorageSnapshotCache cache2(snap2->block_id, get_db_proxy());

  auto fresh = cache2.try_get_snapshot_item(addrkey);
  ASSERT_TRUE(fresh.has_value());
  ASSERT_NE(fresh.value(), nullptr);
  EXPECT_EQ(fresh.value()->bytestring(), make_bytestring("new-value"));

  EXPECT_TRUE(
      get_db_proxy().close_viewing_snapshot(snap2->block_id).has_value());
  EXPECT_TRUE(
      get_db_proxy().close_viewing_snapshot(snap1->block_id).has_value());
}

TEST_F(DirectStalenessTest, FreshViewingSnapshotSeesNewlyDeployedContract)
{
  auto contract_addr = make_contract_address(11);
  auto missing_hash =
      sha256(std::string("contract-not-yet-deployed"));

  advance_block();

  auto snap1 = get_db_proxy().open_viewing_snapshot();
  ASSERT_TRUE(snap1.has_value());
  StorageSnapshotCache cache1(snap1->block_id, get_db_proxy());

  // Warm negative contract lookup.
  auto warm = cache1.try_get_contract_by_hash(missing_hash);
  ASSERT_TRUE(warm.has_value());
  EXPECT_EQ(warm.value(), nullptr);

  // Warm negative deployment lookup.
  auto warm_deploy = cache1.try_get_deployed_contract_hash(contract_addr);
  ASSERT_TRUE(warm_deploy.has_value());
  EXPECT_FALSE(warm_deploy.value().has_value());

  // Now deploy the contract and advance.
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  advance_block();

  // Stale cache still returns nulls.
  auto stale_contract = cache1.try_get_contract_by_hash(missing_hash);
  ASSERT_TRUE(stale_contract.has_value());
  EXPECT_EQ(stale_contract.value(), nullptr);

  auto stale_deploy = cache1.try_get_deployed_contract_hash(contract_addr);
  ASSERT_TRUE(stale_deploy.has_value());
  EXPECT_FALSE(stale_deploy.value().has_value());

  // Fresh viewing snapshot sees the deployment.
  auto snap2 = get_db_proxy().open_viewing_snapshot();
  ASSERT_TRUE(snap2.has_value());
  StorageSnapshotCache cache2(snap2->block_id, get_db_proxy());

  auto fresh_deploy = cache2.try_get_deployed_contract_hash(contract_addr);
  ASSERT_TRUE(fresh_deploy.has_value());
  EXPECT_TRUE(fresh_deploy.value().has_value());

  auto deployed_hash = sha256(*contract);
  auto fresh_contract = cache2.try_get_contract_by_hash(deployed_hash);
  ASSERT_TRUE(fresh_contract.has_value());
  EXPECT_NE(fresh_contract.value(), nullptr);

  EXPECT_TRUE(
      get_db_proxy().close_viewing_snapshot(snap2->block_id).has_value());
  EXPECT_TRUE(
      get_db_proxy().close_viewing_snapshot(snap1->block_id).has_value());
}

TEST_F(DirectStalenessTest, FreshViewingSnapshotSeesNewDbTip)
{
  advance_block();

  auto snap1 = get_db_proxy().open_viewing_snapshot();
  ASSERT_TRUE(snap1.has_value());
  StorageSnapshotCache cache1(snap1->block_id, get_db_proxy());

  auto warm_tip = cache1.try_get_db_tip();
  ASSERT_TRUE(warm_tip.has_value());
  BlockHeight old_height = warm_tip.value()->height;

  advance_block();

  // Stale cache still returns old height.
  auto stale_tip = cache1.try_get_db_tip();
  ASSERT_TRUE(stale_tip.has_value());
  EXPECT_EQ(stale_tip.value()->height, old_height);

  // Fresh viewing snapshot sees incremented height.
  auto snap2 = get_db_proxy().open_viewing_snapshot();
  ASSERT_TRUE(snap2.has_value());
  StorageSnapshotCache cache2(snap2->block_id, get_db_proxy());

  auto fresh_tip = cache2.try_get_db_tip();
  ASSERT_TRUE(fresh_tip.has_value());
  EXPECT_GT(fresh_tip.value()->height, old_height);

  EXPECT_TRUE(
      get_db_proxy().close_viewing_snapshot(snap2->block_id).has_value());
  EXPECT_TRUE(
      get_db_proxy().close_viewing_snapshot(snap1->block_id).has_value());
}

} // namespace hogd
