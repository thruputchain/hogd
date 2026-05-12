#include <gtest/gtest.h>

#include <string_view>
#include <utility>

#include "db_sched.hh"
#include "ffi_enums.hh"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/common/status.h"
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

  SiblingBlocks create_and_commit_sibling()
  {
    BlockID block_a = get_block_number();
    BlockID parent_id = get_db_tip().id;

    constexpr BlockID kSiblingOffset = 1000;
    BlockID block_b = block_a + kSiblingOffset;
    auto r = get_coord_client().dbsched_create_snapshot(
        parent_id, block_b, BlockExecutionMode::kBlockExecutionModeProposal);
    EXPECT_EQ(r, DbSchedResult::DbSchedResultOK);

    auto close = get_coord_client().dbsched_close_snapshot(block_a);
    EXPECT_EQ(close.result(), DbSchedResult::DbSchedResultOK);

    auto commit = get_coord_client().dbsched_commit_snapshot(
        block_a,
        // NOLINTNEXTLINE(bugprone-argument-comment)
        /*block_hash=*/BlockHash{}, /*events_hash=*/SHA256Hash{});
    EXPECT_EQ(commit.result(), DbSchedResult::DbSchedResultOK);

    return {.block_a = block_a, .block_b = block_b};
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

} // namespace hogd
