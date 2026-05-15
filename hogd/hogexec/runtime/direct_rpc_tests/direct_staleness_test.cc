#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <utility>

#include "db_sched.hh"
#include "ffi_enums.hh"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/borrowed_contract_batch.h"
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

} // namespace hogd
