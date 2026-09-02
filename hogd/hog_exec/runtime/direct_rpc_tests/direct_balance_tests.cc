#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

class DirectBalanceTest : public ::testing::TestWithParam<TestDB>,
                          public NativeContractTestFramework {
public:
  DirectBalanceTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

  static auto make_addrkey(ContractAddress contract_addr, std::string_view key)
  {
    return make_contract_data_addrkey(contract_addr, key);
  }

  auto make_new_per_tx_db_proxy()
  {
    return NativeContractTestFramework::make_new_per_tx_db_proxy(UINT64_MAX);
  }
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// request_balance_change must carry forward total_locked_storage_amount from
// the parent depth entry.  Previously it zeroed the field, which lost storage
// costs locked at an earlier depth whenever a balance transfer occurred at a
// deeper depth in the same transaction.
TEST_P(DirectBalanceTest, BalanceTransferAtNewDepthPreservesLockedStorage)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto contract_addr = make_contract_address(0);
  auto recipient_addr = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);
  setup_grant_storage_token(recipient_addr, kInitialBalance);

  auto key = make_addrkey(contract_addr, "some_data");

  auto tx = make_new_per_tx_db_proxy();

  // Frame 1: write a bytestring — locks storage cost on contract_addr
  {
    tx.add_delta(key, make_bytestring_delta("hello world, this is some data"),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Frame 2 (nested): balance transfer FROM contract_addr.
  // request_balance_change must copy total_locked_storage_amount from the
  // parent depth entry so that storage costs from frame 1 are preserved.
  constexpr int128_t kTransferAmount = 100;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(contract_addr),
                 make_nnint_add_delta(-kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::None));
    tx.add_delta(make_free_native_token_balance_addrkey(recipient_addr),
                 make_nnint_add_delta(kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction()().has_value());

  // Reference: same operations but all in one frame (no depth mismatch)
  auto ref_addr = make_contract_address(2);
  auto ref_recipient = make_contract_address(3);
  setup_grant_storage_token(ref_addr, kInitialBalance);
  setup_grant_storage_token(ref_recipient, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_addr, "some_data"),
                     make_bytestring_delta("hello world, this is some data"),
                     std::to_underlying(StorageDeltaFlags::None));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_addr),
                     make_nnint_add_delta(-kTransferAmount),
                     std::to_underlying(StorageDeltaFlags::None));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_recipient),
                     make_nnint_add_delta(kTransferAmount),
                     std::to_underlying(StorageDeltaFlags::None));
    auto r = tx_ref.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction()().has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_addr));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  // Both should have: initial - storage_cost - transfer_amount
  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Balance transfer at new depth should not lose locked storage cost";
}

// Storage at depth 1, transfer at depth 2, more storage at depth 3.
// Exercises the full chain: lock_storage -> request_balance_change ->
// lock_storage each needing to carry forward from the previous depth.
TEST_P(DirectBalanceTest, ThreeDepthsStorageTransferStorage)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(150);
  auto addr_B = make_contract_address(151);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: write first storage key
  {
    tx.add_delta(make_addrkey(addr_A, "key1"), make_bytestring_delta("data1"),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: transfer
  constexpr int128_t kTransfer = 500;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::None));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 3: write second storage key (same address)
  {
    tx.add_delta(make_addrkey(addr_A, "key2"), make_bytestring_delta("data2"),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction()().has_value());

  // Reference: all three operations in one batch
  auto ref_A = make_contract_address(152);
  auto ref_B = make_contract_address(153);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_B, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key1"),
                     make_bytestring_delta("data1"),
                     std::to_underlying(StorageDeltaFlags::None));
    tx_ref.add_delta(make_addrkey(ref_A, "key2"),
                     make_bytestring_delta("data2"),
                     std::to_underlying(StorageDeltaFlags::None));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransfer),
                     std::to_underlying(StorageDeltaFlags::None));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(kTransfer),
                     std::to_underlying(StorageDeltaFlags::None));
    auto r = tx_ref.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction()().has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Storage-transfer-storage across 3 depths should match single-depth";
}

// Five depths with a partial rollback: storage at d1, transfer at d2,
// storage at d3, transfer at d4, then pop back to d2 and push a new
// transfer at d3.  Tests that rollback + re-push correctly preserves
// the storage lock from d1.
TEST_P(DirectBalanceTest, FiveDepthsPartialRollback)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(30);
  auto addr_B = make_contract_address(31);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: storage key1
  {
    tx.add_delta(make_addrkey(addr_A, "key1"), make_bytestring_delta("data1"),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: transfer 100
  constexpr int128_t kTransfer1 = 100;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer1),
                 std::to_underlying(StorageDeltaFlags::None));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer1),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 3: storage key2
  {
    tx.add_delta(make_addrkey(addr_A, "key2"), make_bytestring_delta("data2"),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 4: transfer cumulative 200 total
  constexpr int128_t kTransfer2 = 200;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer2),
                 std::to_underlying(StorageDeltaFlags::None));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer2),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Rollback depths 3 and 4 (back to depth 2)
  ASSERT_TRUE(tx.pop_subtransactions(2)().has_value());

  // Depth 3: new transfer of 300
  constexpr int128_t kTransfer3 = 300;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer3),
                 std::to_underlying(StorageDeltaFlags::None));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer3),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction()().has_value());

  // Reference: key1 storage + 300 transfer (key2 was rolled back, depth 2
  // transfer was also rolled back, replaced by depth 3 transfer of 300)
  auto ref_A = make_contract_address(32);
  auto ref_B = make_contract_address(33);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_B, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key1"),
                     make_bytestring_delta("data1"),
                     std::to_underlying(StorageDeltaFlags::None));
    auto r = tx_ref.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransfer3),
                     std::to_underlying(StorageDeltaFlags::None));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(kTransfer3),
                     std::to_underlying(StorageDeltaFlags::None));
    auto r = tx_ref.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction()().has_value());

  advance_block();

  auto bal_test_A = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref_A = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test_A.has_value());
  ASSERT_TRUE(bal_ref_A.has_value());

  EXPECT_EQ(from_xdr(bal_test_A->nnint()), from_xdr(bal_ref_A->nnint()))
      << "5-depth partial rollback should preserve only key1 + final transfer";
}

// Two independent transactions in the same block, each doing
// storage-at-depth-1 + transfer-at-depth-2.  Ensures there's no
// cross-contamination between transactions sharing the same block.
TEST_P(DirectBalanceTest, TwoIndependentTransactionsInSameBlock)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(10);
  auto addr_B = make_contract_address(11);
  auto addr_C = make_contract_address(12);
  auto addr_D = make_contract_address(13);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);
  setup_grant_storage_token(addr_C, kInitialBalance);
  setup_grant_storage_token(addr_D, kInitialBalance);

  // TX1: A writes storage at depth 1, transfers to B at depth 2
  {
    auto tx1 = make_new_per_tx_db_proxy();
    {
      tx1.add_delta(make_addrkey(addr_A, "key"), make_bytestring_delta("data"),
                    std::to_underlying(StorageDeltaFlags::None));
      auto r = tx1.try_push_current_subtransaction()();
      ASSERT_TRUE(r.has_value());
      ASSERT_TRUE(r.value());
    }
    constexpr int128_t kTransfer1 = 500;
    {
      tx1.add_delta(make_free_native_token_balance_addrkey(addr_A),
                    make_nnint_add_delta(-kTransfer1),
                    std::to_underlying(StorageDeltaFlags::None));
      tx1.add_delta(make_free_native_token_balance_addrkey(addr_B),
                    make_nnint_add_delta(kTransfer1),
                    std::to_underlying(StorageDeltaFlags::None));
      auto r = tx1.try_push_current_subtransaction()();
      ASSERT_TRUE(r.has_value());
      ASSERT_TRUE(r.value());
    }
    ASSERT_TRUE(tx1.commit_transaction()().has_value());
  }

  // TX2: C writes storage at depth 1, transfers to D at depth 2
  {
    auto tx2 = make_new_per_tx_db_proxy();
    {
      tx2.add_delta(make_addrkey(addr_C, "key"),
                    make_bytestring_delta("different_data"),
                    std::to_underlying(StorageDeltaFlags::None));
      auto r = tx2.try_push_current_subtransaction()();
      ASSERT_TRUE(r.has_value());
      ASSERT_TRUE(r.value());
    }
    constexpr int128_t kTransfer2 = 300;
    {
      tx2.add_delta(make_free_native_token_balance_addrkey(addr_C),
                    make_nnint_add_delta(-kTransfer2),
                    std::to_underlying(StorageDeltaFlags::None));
      tx2.add_delta(make_free_native_token_balance_addrkey(addr_D),
                    make_nnint_add_delta(kTransfer2),
                    std::to_underlying(StorageDeltaFlags::None));
      auto r = tx2.try_push_current_subtransaction()();
      ASSERT_TRUE(r.has_value());
      ASSERT_TRUE(r.value());
    }
    ASSERT_TRUE(tx2.commit_transaction()().has_value());
  }

  advance_block();

  // Both transactions should have correct balances independently
  auto bal_A = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_C = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_C));
  ASSERT_TRUE(bal_A.has_value());
  ASSERT_TRUE(bal_C.has_value());

  // A and C had same initial balance but different transfers and data sizes.
  // A: stored "data" (4 bytes, cost 12040), transferred 500 = -12540 total.
  // C: stored "different_data" (14 bytes, cost 12140), transferred 300 =
  // -12440. So C should have more remaining (lost less total).
  EXPECT_GE(from_xdr(bal_A->nnint()), 0);
  EXPECT_GE(from_xdr(bal_C->nnint()), 0);
  EXPECT_NE(from_xdr(bal_A->nnint()), from_xdr(bal_C->nnint()))
      << "Different operations should produce different balances";
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, DirectBalanceTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

} // namespace hogd
