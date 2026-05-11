#include <gtest/gtest.h>

#include <string_view>
#include <utility>

#include "db_sched.hh"
#include "hogd/base/types.h"
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
};

// Verify that a snapshot read through a sibling block that was invalidated when
// another sibling committed returns SystemErrorCode::Abandon (via kRPCBadBlockID).
TEST_F(DirectStalenessTest, SnapshotReadOnStaleBlockReturnsAbandon)
{
  // 1. Commit some data so the db has a real parent tip.
  auto contract_addr = make_contract_address(0);
  constexpr int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto key = make_contract_data_addrkey(contract_addr, "test_key");

  {
    auto tx = make_new_per_tx_db_proxy(0);
    tx.add_delta(key, make_nnint_set_add_delta(0, 42),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
    ASSERT_TRUE(tx.commit_transaction().has_value());
  }

  // advance_block() closes+commits the current block and opens a new one.
  // After this:
  //   dbtip_block_id_ = old block_number_ (committed)
  //   block_number_   = new open block (block A)
  advance_block();

  // 2. block_number_ is now block A (the currently open block).
  //    Its parent is get_db_tip().id.
  BlockID block_a = get_block_number();
  DbTip parent_tip = get_db_tip();
  BlockID parent_id = parent_tip.id;

  // 3. Create a sibling block B from the same parent.
  constexpr BlockID kSiblingOffset = 1000;
  BlockID block_b = block_a + kSiblingOffset;
  {
    auto r = get_coord_client().dbsched_create_snapshot(
        parent_id, block_b,
        BlockExecutionMode::kBlockExecutionModeProposal);
    ASSERT_EQ(r, DbSchedResult::DbSchedResultOK);
  }

  // 4. Verify we can read through block_b before any sibling commits.
  {
    auto res = get_db_proxy().get_snapshot_item(block_b, key);
    ASSERT_TRUE(res.has_value())
        << "Expected successful read through live sibling block_b";
  }

  // 5. Close and commit block A (the framework's current block).
  //    This invalidates block B as a sibling.
  {
    auto r = get_coord_client().dbsched_close_snapshot(block_a);
    ASSERT_EQ(r.result(), DbSchedResult::DbSchedResultOK);
  }
  {
    auto r = get_coord_client().dbsched_commit_snapshot(
        block_a,
        // NOLINTNEXTLINE(bugprone-argument-comment)
        /*block_hash=*/BlockHash{}, /*events_hash=*/SHA256Hash{});
    ASSERT_EQ(r.result(), DbSchedResult::DbSchedResultOK);
  }

  // 6. Now block_b is stale (its sibling block_a was committed).
  //    A snapshot read through block_b must return kRPCBadBlockID, which
  //    DatabaseProxy maps to SystemErrorCode::Abandon.
  {
    auto res = get_db_proxy().get_snapshot_item(block_b, key);
    ASSERT_FALSE(res.has_value())
        << "Expected read through stale block_b to fail with Abandon";
    EXPECT_EQ(res.error().code(), SystemErrorCode::Abandon);
  }
}

} // namespace hogd
