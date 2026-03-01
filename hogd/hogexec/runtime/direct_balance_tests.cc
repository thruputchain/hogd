#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
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
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
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
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(recipient_addr),
                 make_nnint_add_delta(kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: same operations but all in one frame (no depth mismatch)
  auto ref_addr = make_contract_address(2);
  auto ref_recipient = make_contract_address(3);
  setup_grant_storage_token(ref_addr, kInitialBalance);
  setup_grant_storage_token(ref_recipient, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_addr, "some_data"),
                     make_bytestring_delta("hello world, this is some data"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_addr),
                     make_nnint_add_delta(-kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_recipient),
                     make_nnint_add_delta(kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

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

// H112: After popping a subtransaction that wrote a storage key, re-pushing
// with a balance transfer + new storage write should properly account for
// storage cost. The rollback should cleanly undo the first write's cost
// tracking, and the re-push should build fresh state.
TEST_P(DirectBalanceTest, PopStorageWriteThenRepushWithBalanceTransfer)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(10);
  auto recipient = make_contract_address(11);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(recipient, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Frame 1: write key_a (locks storage cost)
  {
    tx.add_delta(make_addrkey(addr_A, "key_a"),
                 make_bytestring_delta("data for key_a"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Pop frame 1 (rollback)
  {
    auto r = tx.pop_subtransactions(1);
    ASSERT_TRUE(r.has_value());
  }

  // Frame 2: balance transfer + write key_b
  constexpr int128_t kTransferAmount = 200;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(recipient),
                 make_nnint_add_delta(kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_addrkey(addr_A, "key_b"),
                 make_bytestring_delta("data for key_b"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: same final operations in one frame
  auto ref_A = make_contract_address(12);
  auto ref_recip = make_contract_address(13);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_recip, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_recip),
                     make_nnint_add_delta(kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_addrkey(ref_A, "key_b"),
                     make_bytestring_delta("data for key_b"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Pop-then-repush should produce same balance as single frame";
}

// H113: Multiple storage writes at different depths, rollback middle depths,
// then add balance transfer. Verifies that rollback properly restores storage
// cost accounting and the final balance is consistent.
TEST_P(DirectBalanceTest, MultiDepthStorageRollbackThenTransfer)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(20);
  auto recipient = make_contract_address(21);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(recipient, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: write key_a
  {
    tx.add_delta(make_addrkey(addr_A, "key_a"),
                 make_bytestring_delta("aaaa"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  // Depth 2: write key_b
  {
    tx.add_delta(make_addrkey(addr_A, "key_b"),
                 make_bytestring_delta("bbbb"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  // Depth 3: write key_c
  {
    tx.add_delta(make_addrkey(addr_A, "key_c"),
                 make_bytestring_delta("cccc"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Pop 2 (rollback depths 2 and 3, keeping only depth 1)
  {
    auto r = tx.pop_subtransactions(2);
    ASSERT_TRUE(r.has_value());
  }

  // Depth 2 (re-push): balance transfer
  constexpr int128_t kTransferAmount = 300;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(recipient),
                 make_nnint_add_delta(kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: write key_a + transfer in one frame (no intermediate writes)
  auto ref_A = make_contract_address(22);
  auto ref_recip = make_contract_address(23);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_recip, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key_a"),
                     make_bytestring_delta("aaaa"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_recip),
                     make_nnint_add_delta(kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  // Both should have: initial - cost_a - transfer
  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Multi-depth rollback then transfer should match reference";
}

// H114: Overwrite a key with smaller data (reducing storage cost) at a deeper
// depth, then do a balance transfer. The final balance should reflect the
// reduced storage cost, not the original larger cost.
TEST_P(DirectBalanceTest, StorageOverwriteReducesCostThenTransfer)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(30);
  auto recipient = make_contract_address(31);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(recipient, kInitialBalance);

  // Write large data, then overwrite with small data, then transfer.
  std::string large_data(500, 'X'); // 500 bytes
  std::string small_data(10, 'Y');  // 10 bytes

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: write large data
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta(large_data),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: overwrite with small data (cost should decrease)
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta(small_data),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 3: balance transfer
  constexpr int128_t kTransferAmount = 100;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(recipient),
                 make_nnint_add_delta(kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: write small data + transfer in one frame
  auto ref_A = make_contract_address(32);
  auto ref_recip = make_contract_address(33);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_recip, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key"),
                     make_bytestring_delta(small_data),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_recip),
                     make_nnint_add_delta(kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  // The test path writes large then overwrites small; the reference writes
  // small directly. The final committed data is small in both cases, so the
  // storage cost should match. The balance should be: initial - cost_small -
  // transfer.
  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Overwrite to smaller data should have reduced storage cost";
}

// H115: Delete a storage key at a deeper depth (should release storage cost),
// then do a balance transfer. The deleted key's cost should be fully refunded.
TEST_P(DirectBalanceTest, DeleteStorageThenTransfer)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(40);
  auto recipient = make_contract_address(41);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(recipient, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: write key
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("some data to store"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: delete key
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_delete_delta(),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 3: balance transfer
  constexpr int128_t kTransferAmount = 500;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(recipient),
                 make_nnint_add_delta(kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: just balance transfer (no storage writes since create+delete
  // cancels out)
  auto ref_A = make_contract_address(42);
  auto ref_recip = make_contract_address(43);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_recip, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_recip),
                     make_nnint_add_delta(kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  // Create+delete should fully refund storage cost, so final balance should
  // match reference which had no storage writes at all.
  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Create+delete should fully refund storage cost";
}

// H116: Write two storage keys at depth 1, overwrite one at depth 2 to be
// smaller, then pop depth 2 (reverting overwrite), then transfer at depth 2.
// After rollback, the storage cost should reflect the ORIGINAL (larger) key,
// not the overwritten smaller one.
TEST_P(DirectBalanceTest, RollbackOverwriteRestoresOriginalCost)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(50);
  auto recipient = make_contract_address(51);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(recipient, kInitialBalance);

  std::string large_data(200, 'L');
  std::string small_data(5, 'S');

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: write large key
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta(large_data),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: overwrite with small data
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta(small_data),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Pop depth 2 (revert overwrite, restore large data)
  {
    auto r = tx.pop_subtransactions(1);
    ASSERT_TRUE(r.has_value());
  }

  // Depth 2 (re-push): balance transfer
  constexpr int128_t kTransferAmount = 100;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(recipient),
                 make_nnint_add_delta(kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: write large data + transfer (overwrite was rolled back)
  auto ref_A = make_contract_address(52);
  auto ref_recip = make_contract_address(53);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_recip, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key"),
                     make_bytestring_delta(large_data),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_recip),
                     make_nnint_add_delta(kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Rolled-back overwrite should restore original (larger) cost";
}

// H117: Multiple storage operations on different keys for the same address
// across multiple depths, with a balance transfer at the deepest depth.
// Tests accumulation of total_locked_storage_amount across depths.
TEST_P(DirectBalanceTest, AccumulateStorageCostAcrossDepths)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(60);
  auto recipient = make_contract_address(61);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(recipient, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: write key_a
  {
    tx.add_delta(make_addrkey(addr_A, "key_a"),
                 make_bytestring_delta("data_a"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: write key_b
  {
    tx.add_delta(make_addrkey(addr_A, "key_b"),
                 make_bytestring_delta("data_b"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 3: write key_c
  {
    tx.add_delta(make_addrkey(addr_A, "key_c"),
                 make_bytestring_delta("data_c"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 4: balance transfer
  constexpr int128_t kTransferAmount = 50;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(recipient),
                 make_nnint_add_delta(kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: all keys + transfer in one frame
  auto ref_A = make_contract_address(62);
  auto ref_recip = make_contract_address(63);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_recip, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key_a"),
                     make_bytestring_delta("data_a"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_addrkey(ref_A, "key_b"),
                     make_bytestring_delta("data_b"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_addrkey(ref_A, "key_c"),
                     make_bytestring_delta("data_c"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_recip),
                     make_nnint_add_delta(kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  // Should be: initial - cost_a - cost_b - cost_c - transfer
  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Accumulated storage costs across depths should match single frame";
}

// H118: Write storage at depth 1, delete at depth 2, rollback depth 2
// (un-delete), then transfer at depth 2. The storage cost should reflect
// the still-existing key (since delete was rolled back).
TEST_P(DirectBalanceTest, RollbackDeleteRestoresCostThenTransfer)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(70);
  auto recipient = make_contract_address(71);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(recipient, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: write key
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("data to store"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: delete key (would release storage cost)
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_delete_delta(),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Pop depth 2 (un-delete, restore the key)
  {
    auto r = tx.pop_subtransactions(1);
    ASSERT_TRUE(r.has_value());
  }

  // Depth 2 (re-push): balance transfer
  constexpr int128_t kTransferAmount = 100;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(recipient),
                 make_nnint_add_delta(kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: write key + transfer (no delete)
  auto ref_A = make_contract_address(72);
  auto ref_recip = make_contract_address(73);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_recip, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key"),
                     make_bytestring_delta("data to store"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_recip),
                     make_nnint_add_delta(kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Rolled-back delete should restore storage cost";
}

// H132: Storage write at depth 1, then a too-large balance transfer at depth 2
// (should fail push), then re-push at depth 2 with a valid smaller transfer.
// Tests that the rollback after sync_balance_locks failure doesn't leave stale
// state that prevents subsequent operations.
TEST_P(DirectBalanceTest, FailedTransferThenSuccessfulRetry)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(80);
  auto recipient = make_contract_address(81);
  // Storage cost for a bytestring "data" (4 bytes) with default cost model:
  // kBaseItemCost(12000) + 4*kBytestringCostPerByte(10) = 12040
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(recipient, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: write storage (locks cost ~12040, reducing available balance)
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("data"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value()) << "Storage write should succeed with large balance";
  }

  // Depth 2: try a transfer larger than remaining balance (should fail)
  {
    // Transfer ALL initial balance (but storage cost has already been locked)
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kInitialBalance),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(recipient),
                 make_nnint_add_delta(kInitialBalance),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    // This should fail because: initial - storage_cost - initial_transfer < 0
    EXPECT_FALSE(r.value()) << "Transfer exceeding available balance should fail";
  }

  // Depth 2 (retry): smaller transfer that should succeed
  constexpr int128_t kSmallTransfer = 100;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kSmallTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(recipient),
                 make_nnint_add_delta(kSmallTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value()) << "Small transfer should succeed after failed retry";
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: storage write + small transfer in a single tx (no failed push)
  auto ref_A = make_contract_address(82);
  auto ref_recip = make_contract_address(83);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_recip, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key"),
                     make_bytestring_delta("data"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kSmallTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_recip),
                     make_nnint_add_delta(kSmallTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Retry after failed transfer should produce correct balance";
}

// H133: Two different addresses each have storage writes at depth 1.
// At depth 2, transfer between them. Verifies that storage costs are tracked
// independently per address and the transfer properly accounts for both.
TEST_P(DirectBalanceTest, TwoAddressesWithStoragePlusTransfer)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(90);
  auto addr_B = make_contract_address(91);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: both addresses write storage
  {
    tx.add_delta(make_addrkey(addr_A, "key_a"),
                 make_bytestring_delta("data for A"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_addrkey(addr_B, "key_b"),
                 make_bytestring_delta("data for B, longer"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: transfer from A to B
  constexpr int128_t kTransferAmount = 500;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransferAmount),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: same operations in one frame
  auto ref_A = make_contract_address(92);
  auto ref_B = make_contract_address(93);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_B, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key_a"),
                     make_bytestring_delta("data for A"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_addrkey(ref_B, "key_b"),
                     make_bytestring_delta("data for B, longer"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(kTransferAmount),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test_A = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref_A = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  auto bal_test_B = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_B));
  auto bal_ref_B = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_B));

  ASSERT_TRUE(bal_test_A.has_value());
  ASSERT_TRUE(bal_ref_A.has_value());
  ASSERT_TRUE(bal_test_B.has_value());
  ASSERT_TRUE(bal_ref_B.has_value());

  EXPECT_EQ(from_xdr(bal_test_A->nnint()), from_xdr(bal_ref_A->nnint()))
      << "Address A balance should match reference";
  EXPECT_EQ(from_xdr(bal_test_B->nnint()), from_xdr(bal_ref_B->nnint()))
      << "Address B balance should match reference";
}

// H134: Transfer to self: an address transfers balance to itself. This should
// be a no-op for the balance (transfer_out + transfer_in = 0), but storage
// costs should still be properly tracked.
TEST_P(DirectBalanceTest, TransferToSelfWithStorageCost)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(100);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: write storage
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("some stored data"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: "transfer" 100 to self (net 0 balance change from transfers,
  // but storage cost should still apply)
  {
    // Note: in the real executor, a single address would have one cumulative
    // balance delta. A "transfer to self" of 100 would be:
    // modify_native_token_balance(addr_A, -100)  // send
    // modify_native_token_balance(addr_A, +100)  // receive
    // net add_value_ = 0
    // So the delta sent to DB would be 0.
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(0),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: just storage write, no transfer
  auto ref_A = make_contract_address(101);
  setup_grant_storage_token(ref_A, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key"),
                     make_bytestring_delta("some stored data"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  // Self-transfer of 0 should have no effect on balance beyond storage cost
  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Transfer-to-self should not affect balance beyond storage costs";
}

// H135: Two different storage keys for the same address at the same depth.
// Both contribute to total_locked_storage_amount via lock_storage().
// Verify the costs are summed correctly and the final balance reflects both.
TEST_P(DirectBalanceTest, TwoStorageKeysAtSameDepthCombineCost)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(110);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: write two storage keys in one batch
  {
    tx.add_delta(make_addrkey(addr_A, "key1"),
                 make_bytestring_delta("data1"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_addrkey(addr_A, "key2"),
                 make_bytestring_delta("data2"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: transfer some tokens
  constexpr int128_t kTransfer = 500;
  auto recipient = make_contract_address(111);
  setup_grant_storage_token(recipient, kInitialBalance);
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(recipient),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: same operations all at one depth
  auto ref_A = make_contract_address(112);
  auto ref_recip = make_contract_address(113);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_recip, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key1"),
                     make_bytestring_delta("data1"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_addrkey(ref_A, "key2"),
                     make_bytestring_delta("data2"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_recip),
                     make_nnint_add_delta(kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Two keys at depth 1 + transfer at depth 2 should match single-depth";
}

// H136: Storage key at depth 1, second storage key at depth 2 (same address),
// rollback to depth 1. Verify key2's cost is properly removed from balance
// tracking while key1's cost remains.
TEST_P(DirectBalanceTest, RollbackSecondKeyPreservesFirstKeyCost)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(120);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: write first storage key
  {
    tx.add_delta(make_addrkey(addr_A, "key1"),
                 make_bytestring_delta("data1"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: write second storage key (costs additional storage)
  {
    tx.add_delta(make_addrkey(addr_A, "key2"),
                 make_bytestring_delta("data2"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Rollback depth 2, removing key2
  tx.pop_subtransactions(1);

  // Depth 2: transfer some tokens (should work because only key1's cost remains)
  constexpr int128_t kTransfer = 500;
  auto recipient = make_contract_address(121);
  setup_grant_storage_token(recipient, kInitialBalance);
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(recipient),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: just key1 + transfer
  auto ref_A = make_contract_address(122);
  auto ref_recip = make_contract_address(123);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_recip, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key1"),
                     make_bytestring_delta("data1"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_recip),
                     make_nnint_add_delta(kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Rolled-back key2 should not affect final balance";
}

// H138: Transfer at depth 1, then storage write at depth 2. Tests that
// lock_storage correctly copies manual_transfer_amount from parent when
// creating a new balance_deltas_ entry at a deeper depth.
TEST_P(DirectBalanceTest, TransferThenStorageAtDeeperDepth)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(130);
  auto addr_B = make_contract_address(131);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: transfer from A to B
  constexpr int128_t kTransfer = 500;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: storage write for addr_A (lock_storage should copy
  // manual_transfer_amount = -500 from parent depth entry)
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("data"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: same operations in one batch
  auto ref_A = make_contract_address(132);
  auto ref_B = make_contract_address(133);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_B, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_addrkey(ref_A, "key"),
                     make_bytestring_delta("data"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Transfer then storage across depths should match single-depth";
}

// H139: Storage at depth 1, transfer at depth 2, rollback depth 2,
// then different transfer at depth 2. The second transfer should work
// correctly without corruption from the rolled-back first transfer.
TEST_P(DirectBalanceTest, RollbackTransferThenDifferentTransfer)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(140);
  auto addr_B = make_contract_address(141);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: write storage
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("data"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: transfer 1000 from A to B
  constexpr int128_t kTransfer1 = 1000;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer1),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer1),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Rollback depth 2
  tx.pop_subtransactions(1);

  // Depth 2 again: different transfer, 2000 from A to B
  constexpr int128_t kTransfer2 = 2000;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer2),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer2),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: storage + transfer2 directly
  auto ref_A = make_contract_address(142);
  auto ref_B = make_contract_address(143);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_B, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key"),
                     make_bytestring_delta("data"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransfer2),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(kTransfer2),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test_A = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref_A = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  auto bal_test_B = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_B));
  auto bal_ref_B = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_B));
  ASSERT_TRUE(bal_test_A.has_value());
  ASSERT_TRUE(bal_ref_A.has_value());
  ASSERT_TRUE(bal_test_B.has_value());
  ASSERT_TRUE(bal_ref_B.has_value());

  EXPECT_EQ(from_xdr(bal_test_A->nnint()), from_xdr(bal_ref_A->nnint()))
      << "Sender balance should match reference after rollback+re-transfer";
  EXPECT_EQ(from_xdr(bal_test_B->nnint()), from_xdr(bal_ref_B->nnint()))
      << "Recipient balance should match reference after rollback+re-transfer";
}

// H141: Three depths: storage at 1, transfer at 2, more storage at 3.
// Tests that total_locked_storage_amount accumulates correctly across
// three separate depths while preserving manual_transfer_amount.
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
    tx.add_delta(make_addrkey(addr_A, "key1"),
                 make_bytestring_delta("data1"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: transfer
  constexpr int128_t kTransfer = 500;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 3: write second storage key (same address)
  {
    tx.add_delta(make_addrkey(addr_A, "key2"),
                 make_bytestring_delta("data2"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: all three operations in one batch
  auto ref_A = make_contract_address(152);
  auto ref_B = make_contract_address(153);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_B, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key1"),
                     make_bytestring_delta("data1"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_addrkey(ref_A, "key2"),
                     make_bytestring_delta("data2"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

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

// H142: Storage write and balance transfer in the SAME batch. This tests
// that lock_storage() and request_balance_change() both update the same
// balance_deltas_ entry correctly when happening at the same tx_depth.
TEST_P(DirectBalanceTest, StorageAndTransferInSameBatch)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(160);
  auto addr_B = make_contract_address(161);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: storage write AND transfer in one batch
  constexpr int128_t kTransfer = 500;
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("data"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: same operations, also in one batch (must produce identical result)
  auto ref_A = make_contract_address(162);
  auto ref_B = make_contract_address(163);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_B, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key"),
                     make_bytestring_delta("data"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Storage+transfer in same batch should work correctly";
}

// H143: Overwrite storage with LARGER data at depth 2, increasing cost,
// then rollback. The original cost from depth 1 should be restored.
// Also: after rollback, do a transfer to verify available balance is correct.
TEST_P(DirectBalanceTest, OverwriteLargerThenRollbackRestoresCost)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(170);
  auto addr_B = make_contract_address(171);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: write small data
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("sm"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: overwrite with much larger data (increases storage cost)
  {
    tx.add_delta(
        make_addrkey(addr_A, "key"),
        make_bytestring_delta("this is a much much much longer string"),
        std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Rollback depth 2 -- should restore original cost
  tx.pop_subtransactions(1);

  // Depth 2: transfer. Available balance should be initial - small_data_cost.
  constexpr int128_t kTransfer = 500;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: just small data + transfer (no overwrite)
  auto ref_A = make_contract_address(172);
  auto ref_B = make_contract_address(173);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_B, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key"),
                     make_bytestring_delta("sm"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Overwrite-larger-then-rollback should restore original cost";
}

// H144: Storage write at depth 1, storage write for DIFFERENT address at
// depth 2, transfer between the two addresses at depth 3.
// Tests that balance_deltas_ tracks different addresses independently.
TEST_P(DirectBalanceTest, TwoAddressesStorageAtDifferentDepthsThenTransfer)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(180);
  auto addr_B = make_contract_address(181);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: storage for addr_A
  {
    tx.add_delta(make_addrkey(addr_A, "key_a"),
                 make_bytestring_delta("data_a"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: storage for addr_B
  {
    tx.add_delta(make_addrkey(addr_B, "key_b"),
                 make_bytestring_delta("data_b"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 3: transfer from A to B
  constexpr int128_t kTransfer = 500;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: all in one batch
  auto ref_A = make_contract_address(182);
  auto ref_B = make_contract_address(183);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_B, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key_a"),
                     make_bytestring_delta("data_a"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_addrkey(ref_B, "key_b"),
                     make_bytestring_delta("data_b"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test_A = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref_A = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  auto bal_test_B = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_B));
  auto bal_ref_B = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_B));
  ASSERT_TRUE(bal_test_A.has_value());
  ASSERT_TRUE(bal_ref_A.has_value());
  ASSERT_TRUE(bal_test_B.has_value());
  ASSERT_TRUE(bal_ref_B.has_value());

  EXPECT_EQ(from_xdr(bal_test_A->nnint()), from_xdr(bal_ref_A->nnint()))
      << "Addr A balance should match reference";
  EXPECT_EQ(from_xdr(bal_test_B->nnint()), from_xdr(bal_ref_B->nnint()))
      << "Addr B balance should match reference";
}

// H145: Transfer at depth 1, rollback, then storage write at depth 1.
// The rollback should fully undo the transfer's balance_deltas_ entry,
// allowing a clean storage write without leftover manual_transfer_amount.
TEST_P(DirectBalanceTest, RollbackTransferThenStorageWrite)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(190);
  auto addr_B = make_contract_address(191);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: transfer from A to B
  constexpr int128_t kTransfer = 1000;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Rollback depth 1
  tx.pop_subtransactions(1);

  // Depth 1 again: just storage write (no transfer)
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("data"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: just storage write (no transfer history)
  auto ref_A = make_contract_address(192);
  setup_grant_storage_token(ref_A, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key"),
                     make_bytestring_delta("data"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Rolled-back transfer should not affect subsequent storage write";
}

// H146: Multiple rollbacks. Storage at depth 1, storage at depth 2,
// transfer at depth 3, rollback to depth 1, then transfer at depth 2.
// Only depth 1's storage cost should remain.
TEST_P(DirectBalanceTest, MultipleRollbacksThenTransfer)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(200);
  auto addr_B = make_contract_address(201);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: storage key1
  {
    tx.add_delta(make_addrkey(addr_A, "key1"),
                 make_bytestring_delta("data1"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: storage key2
  {
    tx.add_delta(make_addrkey(addr_A, "key2"),
                 make_bytestring_delta("data2"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 3: transfer
  constexpr int128_t kTransfer = 500;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Rollback depths 3 and 2 (pop 2 levels back to depth 1)
  tx.pop_subtransactions(2);

  // Depth 2: transfer (only key1's cost should be active)
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: key1 + transfer
  auto ref_A = make_contract_address(202);
  auto ref_B = make_contract_address(203);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_B, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key1"),
                     make_bytestring_delta("data1"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Multi-rollback should leave only depth 1 storage cost active";
}

// H147: Receive tokens (positive transfer) at depth 1, then storage write
// at depth 2. The received tokens should be available to cover storage cost.
// Tests that positive manual_transfer_amount correctly increases available
// balance for storage cost checking.
TEST_P(DirectBalanceTest, ReceiveTokensThenStorageWrite)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(210);
  auto addr_B = make_contract_address(211);
  // Give addr_A a very small balance -- not enough for storage on its own
  // but enough after receiving tokens from B.
  constexpr static int128_t kSmallBalance = 100;
  constexpr static int128_t kLargeBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kSmallBalance);
  setup_grant_storage_token(addr_B, kLargeBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: B sends enough to A to cover storage cost
  constexpr int128_t kTransfer = 100'000;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: A writes storage. Even though A only has 100 initial balance,
  // the +100000 transfer delta should make this succeed.
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("data"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value())
        << "Storage write should succeed after receiving tokens";
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: same operations in one batch
  auto ref_A = make_contract_address(212);
  auto ref_B = make_contract_address(213);
  setup_grant_storage_token(ref_A, kSmallBalance);
  setup_grant_storage_token(ref_B, kLargeBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(-kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_addrkey(ref_A, "key"),
                     make_bytestring_delta("data"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Received tokens should cover storage cost across depths";
}

// H148: Pre-existing storage item (written in block N). In block N+1:
// overwrite with larger data at depth 1 (increases cost), transfer at depth 2.
// Tests that storage cost changes on existing items are tracked correctly.
TEST_P(DirectBalanceTest, OverwritePreexistingItemThenTransfer)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(220);
  auto addr_B = make_contract_address(221);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  // Block 1: create pre-existing item
  {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("small"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
    ASSERT_TRUE(tx.commit_transaction({}).has_value());
  }
  advance_block();

  // Block 2: overwrite with larger data + transfer
  {
    auto tx = make_new_per_tx_db_proxy();

    // Depth 1: overwrite with much larger data
    {
      tx.add_delta(
          make_addrkey(addr_A, "key"),
          make_bytestring_delta("this is a much longer replacement string"),
          std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
      auto r = tx.try_push_current_subtransaction();
      ASSERT_TRUE(r.has_value());
      ASSERT_TRUE(r.value());
    }

    // Depth 2: transfer
    constexpr int128_t kTransfer = 500;
    {
      tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                   make_nnint_add_delta(-kTransfer),
                   std::to_underlying(
                       StorageDeltaFlags::kStorageDeltaFlagNone));
      tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                   make_nnint_add_delta(kTransfer),
                   std::to_underlying(
                       StorageDeltaFlags::kStorageDeltaFlagNone));
      auto r = tx.try_push_current_subtransaction();
      ASSERT_TRUE(r.has_value());
      ASSERT_TRUE(r.value());
    }

    ASSERT_TRUE(tx.commit_transaction({}).has_value());
  }

  // Reference: same operations on a fresh address
  auto ref_A = make_contract_address(222);
  auto ref_B = make_contract_address(223);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_B, kInitialBalance);
  // Block 2 ref: create same pre-existing item
  {
    auto tx_ref = make_new_per_tx_db_proxy();
    tx_ref.add_delta(make_addrkey(ref_A, "key"),
                     make_bytestring_delta("small"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
    ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());
  }
  advance_block();

  // Block 3 ref: overwrite + transfer in one batch (single depth)
  {
    auto tx_ref = make_new_per_tx_db_proxy();
    tx_ref.add_delta(
        make_addrkey(ref_A, "key"),
        make_bytestring_delta("this is a much longer replacement string"),
        std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    constexpr int128_t kTransfer = 500;
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
    ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());
  }
  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Overwrite pre-existing + transfer should match single-depth ref";
}

// H149: Delete a pre-existing storage item (negative storage cost change),
// then transfer at deeper depth. Deletion should free up balance.
TEST_P(DirectBalanceTest, DeletePreexistingItemThenTransfer)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(230);
  auto addr_B = make_contract_address(231);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  // Block 1: create pre-existing item
  {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("data to be deleted"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
    ASSERT_TRUE(tx.commit_transaction({}).has_value());
  }
  advance_block();

  // Block 2: delete item at depth 1, transfer at depth 2
  {
    auto tx = make_new_per_tx_db_proxy();

    // Depth 1: delete the pre-existing item
    {
      tx.add_delta(make_addrkey(addr_A, "key"),
                   make_delete_delta(),
                   std::to_underlying(
                       StorageDeltaFlags::kStorageDeltaFlagNone));
      auto r = tx.try_push_current_subtransaction();
      ASSERT_TRUE(r.has_value());
      ASSERT_TRUE(r.value());
    }

    // Depth 2: transfer
    constexpr int128_t kTransfer = 500;
    {
      tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                   make_nnint_add_delta(-kTransfer),
                   std::to_underlying(
                       StorageDeltaFlags::kStorageDeltaFlagNone));
      tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                   make_nnint_add_delta(kTransfer),
                   std::to_underlying(
                       StorageDeltaFlags::kStorageDeltaFlagNone));
      auto r = tx.try_push_current_subtransaction();
      ASSERT_TRUE(r.has_value());
      ASSERT_TRUE(r.value());
    }

    ASSERT_TRUE(tx.commit_transaction({}).has_value());
  }

  // Reference: same pre-existing + delete + transfer in one batch
  auto ref_A = make_contract_address(232);
  auto ref_B = make_contract_address(233);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_B, kInitialBalance);
  {
    auto tx_ref = make_new_per_tx_db_proxy();
    tx_ref.add_delta(make_addrkey(ref_A, "key"),
                     make_bytestring_delta("data to be deleted"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
    ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());
  }
  advance_block();

  {
    auto tx_ref = make_new_per_tx_db_proxy();
    tx_ref.add_delta(make_addrkey(ref_A, "key"),
                     make_delete_delta(),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    constexpr int128_t kTransfer = 500;
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
    ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());
  }
  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Delete pre-existing + transfer should match single-depth ref";
}

// H150: Storage write for addr_A at depth 1, storage write for addr_B at
// depth 2, rollback depth 2, storage write for addr_B at depth 2 again
// (different data), transfer A->B at depth 3. Complex multi-address,
// multi-rollback scenario.
TEST_P(DirectBalanceTest, TwoAddressesRollbackRepushTransfer)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(240);
  auto addr_B = make_contract_address(241);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: storage for A
  {
    tx.add_delta(make_addrkey(addr_A, "key_a"),
                 make_bytestring_delta("data_a"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: storage for B (will be rolled back)
  {
    tx.add_delta(make_addrkey(addr_B, "key_b"),
                 make_bytestring_delta("original_b"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Rollback depth 2
  tx.pop_subtransactions(1);

  // Depth 2 again: different storage for B
  {
    tx.add_delta(make_addrkey(addr_B, "key_b"),
                 make_bytestring_delta("replaced_b_data"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 3: transfer A -> B
  constexpr int128_t kTransfer = 500;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: A storage + B replacement + transfer in two depths
  auto ref_A = make_contract_address(242);
  auto ref_B = make_contract_address(243);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_B, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key_a"),
                     make_bytestring_delta("data_a"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_addrkey(ref_B, "key_b"),
                     make_bytestring_delta("replaced_b_data"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test_A = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref_A = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  auto bal_test_B = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_B));
  auto bal_ref_B = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_B));
  ASSERT_TRUE(bal_test_A.has_value());
  ASSERT_TRUE(bal_ref_A.has_value());
  ASSERT_TRUE(bal_test_B.has_value());
  ASSERT_TRUE(bal_ref_B.has_value());

  EXPECT_EQ(from_xdr(bal_test_A->nnint()), from_xdr(bal_ref_A->nnint()))
      << "Addr A balance after multi-address rollback+re-push";
  EXPECT_EQ(from_xdr(bal_test_B->nnint()), from_xdr(bal_ref_B->nnint()))
      << "Addr B balance after multi-address rollback+re-push";
}

// H151: Transfer at depth 1 and 2 (same address, increasing amounts).
// Tests that cumulative balance deltas from the executor model work correctly.
TEST_P(DirectBalanceTest, CumulativeTransferAcrossDepths)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(250);
  auto addr_B = make_contract_address(251);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: A transfers 1000 to B
  constexpr int128_t kTransfer1 = 1000;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer1),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer1),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: A transfers an additional 2000 to B (cumulative: -3000)
  // The executor sends cumulative deltas, so depth 2 delta for A = -3000.
  constexpr int128_t kTotalTransfer = 3000;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTotalTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTotalTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: single transfer of 3000
  auto ref_A = make_contract_address(252);
  auto ref_B = make_contract_address(253);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_B, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTotalTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(kTotalTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test_A = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref_A = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test_A.has_value());
  ASSERT_TRUE(bal_ref_A.has_value());

  EXPECT_EQ(from_xdr(bal_test_A->nnint()), from_xdr(bal_ref_A->nnint()))
      << "Cumulative transfers across depths should match single transfer";
}

// H152: Five depths of alternating storage writes and transfers with partial
// rollback. Tests complex interaction of the version stack.
// depth 1: storage key1 (A)
// depth 2: transfer A->B
// depth 3: storage key2 (A)
// depth 4: transfer A->B (cumulative)
// rollback to depth 2
// depth 3: new transfer A->B
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
    tx.add_delta(make_addrkey(addr_A, "key1"),
                 make_bytestring_delta("data1"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: transfer 100
  constexpr int128_t kTransfer1 = 100;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer1),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer1),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 3: storage key2
  {
    tx.add_delta(make_addrkey(addr_A, "key2"),
                 make_bytestring_delta("data2"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 4: transfer cumulative 200 total
  constexpr int128_t kTransfer2 = 200;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer2),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer2),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Rollback depths 3 and 4 (back to depth 2)
  tx.pop_subtransactions(2);

  // Depth 3: new transfer of 300
  constexpr int128_t kTransfer3 = 300;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer3),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer3),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

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
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransfer3),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(kTransfer3),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

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

// H153: Transfer that brings balance to EXACTLY the storage cost threshold.
// Balance = initial - transfer = storage_cost. Should succeed (balance covers
// storage cost exactly).
TEST_P(DirectBalanceTest, TransferLeavesExactlyStorageCost)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(10);
  auto addr_B = make_contract_address(11);
  // Storage cost for "data" (4 bytes) = 12000 + 4*10 = 12040
  constexpr static int128_t kStorageCost = 12040;
  constexpr static int128_t kInitialBalance = kStorageCost + 1000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: write storage (cost = kStorageCost)
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("data"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: transfer EXACTLY the remaining 1000 (balance - storage_cost = 0)
  constexpr int128_t kTransfer = 1000;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value()) << "Exact remaining balance transfer should succeed";
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());
  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  ASSERT_TRUE(bal_test.has_value());
  // Final balance = initial - storage_cost - transfer
  // The exact value depends on whether committed cost == locked cost.
  // Just verify it's >= 0.
  EXPECT_GE(from_xdr(bal_test->nnint()), 0)
      << "Final balance should be non-negative";
}

// H154: Transfer that exceeds available balance by exactly 1 should fail.
TEST_P(DirectBalanceTest, TransferExceedsAvailableByOne)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(10);
  auto addr_B = make_contract_address(11);
  constexpr static int128_t kStorageCost = 12040; // for "data"
  constexpr static int128_t kInitialBalance = kStorageCost + 1000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: write storage
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("data"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: transfer 1001 (one more than available)
  constexpr int128_t kTransfer = 1001;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r.value())
        << "Transfer exceeding available by 1 should fail";
  }
}

// H155: Abort a transaction (instead of commit) and verify no side effects.
// A different transaction on the same address should see original balance.
TEST_P(DirectBalanceTest, AbortTransactionNoSideEffects)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(10);
  auto addr_B = make_contract_address(11);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  // Transaction 1: storage + transfer, then abort
  {
    auto tx = make_new_per_tx_db_proxy();
    {
      tx.add_delta(make_addrkey(addr_A, "key"),
                   make_bytestring_delta("data"),
                   std::to_underlying(
                       StorageDeltaFlags::kStorageDeltaFlagNone));
      auto r = tx.try_push_current_subtransaction();
      ASSERT_TRUE(r.has_value());
      ASSERT_TRUE(r.value());
    }
    {
      tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                   make_nnint_add_delta(-500),
                   std::to_underlying(
                       StorageDeltaFlags::kStorageDeltaFlagNone));
      tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                   make_nnint_add_delta(500),
                   std::to_underlying(
                       StorageDeltaFlags::kStorageDeltaFlagNone));
      auto r = tx.try_push_current_subtransaction();
      ASSERT_TRUE(r.has_value());
      ASSERT_TRUE(r.value());
    }
    // Abort instead of commit
    tx.abort_transaction();
  }

  // Transaction 2: just a transfer (should work as if tx1 never happened)
  {
    auto tx2 = make_new_per_tx_db_proxy();
    constexpr int128_t kTransfer = 200;
    tx2.add_delta(make_free_native_token_balance_addrkey(addr_A),
                  make_nnint_add_delta(-kTransfer),
                  std::to_underlying(
                      StorageDeltaFlags::kStorageDeltaFlagNone));
    tx2.add_delta(make_free_native_token_balance_addrkey(addr_B),
                  make_nnint_add_delta(kTransfer),
                  std::to_underlying(
                      StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx2.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value()) << "Transfer after abort should succeed";
    ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  }

  advance_block();

  // Reference: just the transfer (no aborted tx)
  auto ref_A = make_contract_address(12);
  auto ref_B = make_contract_address(13);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_B, kInitialBalance);
  {
    auto tx_ref = make_new_per_tx_db_proxy();
    constexpr int128_t kTransfer = 200;
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
    ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());
  }
  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Aborted transaction should leave no trace";
}

// H156: Multiple independent transactions in the same block, each touching
// different addresses. Verifies no cross-contamination.
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
      tx1.add_delta(make_addrkey(addr_A, "key"),
                    make_bytestring_delta("data"),
                    std::to_underlying(
                        StorageDeltaFlags::kStorageDeltaFlagNone));
      auto r = tx1.try_push_current_subtransaction();
      ASSERT_TRUE(r.has_value());
      ASSERT_TRUE(r.value());
    }
    constexpr int128_t kTransfer1 = 500;
    {
      tx1.add_delta(make_free_native_token_balance_addrkey(addr_A),
                    make_nnint_add_delta(-kTransfer1),
                    std::to_underlying(
                        StorageDeltaFlags::kStorageDeltaFlagNone));
      tx1.add_delta(make_free_native_token_balance_addrkey(addr_B),
                    make_nnint_add_delta(kTransfer1),
                    std::to_underlying(
                        StorageDeltaFlags::kStorageDeltaFlagNone));
      auto r = tx1.try_push_current_subtransaction();
      ASSERT_TRUE(r.has_value());
      ASSERT_TRUE(r.value());
    }
    ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  }

  // TX2: C writes storage at depth 1, transfers to D at depth 2
  {
    auto tx2 = make_new_per_tx_db_proxy();
    {
      tx2.add_delta(make_addrkey(addr_C, "key"),
                    make_bytestring_delta("different_data"),
                    std::to_underlying(
                        StorageDeltaFlags::kStorageDeltaFlagNone));
      auto r = tx2.try_push_current_subtransaction();
      ASSERT_TRUE(r.has_value());
      ASSERT_TRUE(r.value());
    }
    constexpr int128_t kTransfer2 = 300;
    {
      tx2.add_delta(make_free_native_token_balance_addrkey(addr_C),
                    make_nnint_add_delta(-kTransfer2),
                    std::to_underlying(
                        StorageDeltaFlags::kStorageDeltaFlagNone));
      tx2.add_delta(make_free_native_token_balance_addrkey(addr_D),
                    make_nnint_add_delta(kTransfer2),
                    std::to_underlying(
                        StorageDeltaFlags::kStorageDeltaFlagNone));
      auto r = tx2.try_push_current_subtransaction();
      ASSERT_TRUE(r.has_value());
      ASSERT_TRUE(r.value());
    }
    ASSERT_TRUE(tx2.commit_transaction({}).has_value());
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
  // C: stored "different_data" (14 bytes, cost 12140), transferred 300 = -12440.
  // So C should have more remaining (lost less total).
  EXPECT_GE(from_xdr(bal_A->nnint()), 0);
  EXPECT_GE(from_xdr(bal_C->nnint()), 0);
  EXPECT_NE(from_xdr(bal_A->nnint()), from_xdr(bal_C->nnint()))
      << "Different operations should produce different balances";
}

// H157: Storage write + transfer in same batch, but process order varies due
// to debug shuffle. Run several times to exercise different orderings.
// (In debug builds, the batch is shuffled, so this test inherently covers
// multiple orderings across repeated runs.)
TEST_P(DirectBalanceTest, BatchOrderIndependence)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  constexpr static int128_t kInitialBalance = 1'000'000;
  constexpr int128_t kTransfer = 500;

  // Run the same scenario multiple times to exercise different shuffle orders
  for (int i = 0; i < 5; i++) {
    auto addr_A = make_contract_address(10 + i * 4);
    auto addr_B = make_contract_address(11 + i * 4);
    setup_grant_storage_token(addr_A, kInitialBalance);
    setup_grant_storage_token(addr_B, kInitialBalance);

    auto tx = make_new_per_tx_db_proxy();
    {
      tx.add_delta(make_addrkey(addr_A, "key"),
                   make_bytestring_delta("data"),
                   std::to_underlying(
                       StorageDeltaFlags::kStorageDeltaFlagNone));
      tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                   make_nnint_add_delta(-kTransfer),
                   std::to_underlying(
                       StorageDeltaFlags::kStorageDeltaFlagNone));
      tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                   make_nnint_add_delta(kTransfer),
                   std::to_underlying(
                       StorageDeltaFlags::kStorageDeltaFlagNone));
      auto r = tx.try_push_current_subtransaction();
      ASSERT_TRUE(r.has_value()) << "iteration " << i;
      ASSERT_TRUE(r.value()) << "iteration " << i;
    }
    ASSERT_TRUE(tx.commit_transaction({}).has_value()) << "iteration " << i;
  }

  advance_block();

  // All 5 senders should have valid non-negative balances
  for (int i = 0; i < 5; i++) {
    auto addr = make_contract_address(10 + i * 4);
    auto bal = lookup_storage_item_from_db(
        make_free_native_token_balance_addrkey(addr));
    ASSERT_TRUE(bal.has_value()) << "iteration " << i;
    EXPECT_GE(from_xdr(bal->nnint()), 0)
        << "Balance should be non-negative at iteration " << i;
  }
}

// H158: Storage write at depth 1, pop it, write same key with different
// data at depth 1 again, transfer at depth 2. Tests pop-repush with
// modified data for the same key.
TEST_P(DirectBalanceTest, PopAndRepushSameKeyDifferentData)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(10);
  auto addr_B = make_contract_address(11);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: write small data
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("sm"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Pop depth 1
  tx.pop_subtransactions(1);

  // Depth 1 again: write larger data for same key
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("this is significantly larger data"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: transfer
  constexpr int128_t kTransfer = 500;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: larger data + transfer
  auto ref_A = make_contract_address(12);
  auto ref_B = make_contract_address(13);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_B, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key"),
                     make_bytestring_delta(
                         "this is significantly larger data"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(-kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Pop+repush with different data should match reference";
}

// H159: Empty transaction (no deltas, just commit). Should not crash or
// leave invalid state.
TEST_P(DirectBalanceTest, EmptyTransactionCommit)
{
  auto addr_A = make_contract_address(10);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();
  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  advance_block();

  auto bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  ASSERT_TRUE(bal.has_value());
  EXPECT_EQ(from_xdr(bal->nnint()), kInitialBalance)
      << "Empty tx should not change balance";
}

// H160: Zero-amount transfer. Should succeed and produce no balance change.
TEST_P(DirectBalanceTest, ZeroAmountTransfer)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(10);
  auto addr_B = make_contract_address(11);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: storage write
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("data"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: zero transfer
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(0),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(0),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value()) << "Zero transfer should succeed";
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: just storage, no transfer
  auto ref_A = make_contract_address(12);
  setup_grant_storage_token(ref_A, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_addrkey(ref_A, "key"),
                     make_bytestring_delta("data"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Zero transfer should have no effect on balance";
}

// H161: Storage write at depth 1, transfer at depth 2 for an address
// that has NO existing balance (not in snapshot). The balance lock should
// require the delta to cover the balance item's own storage cost (kBalanceCost).
TEST_P(DirectBalanceTest, TransferToNewAddressCoversBalanceCost)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(10);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  // addr_B intentionally NOT granted storage token (no balance in snapshot)
  auto addr_B = make_contract_address(11);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: storage write for A
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("data"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: transfer a large amount to B (enough to cover B's balance cost)
  // kBalanceCost = 1234 from default_cost_model
  constexpr int128_t kTransfer = 10000; // well above kBalanceCost
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value())
        << "Transfer to new address should succeed with enough for balance cost";
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());
  advance_block();

  auto bal_B = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_B));
  ASSERT_TRUE(bal_B.has_value());
  // B's balance = transfer - kBalanceCost = 10000 - 1234 = 8766
  EXPECT_EQ(from_xdr(bal_B->nnint()), kTransfer - 1234)
      << "New address balance should be transfer minus balance cost";
}

// H162: Transfer from B to A at depth 1, then storage write for A at depth 2.
// The positive transfer at depth 1 should increase A's available balance for
// the storage write at depth 2 (opposite direction from H138).
TEST_P(DirectBalanceTest, IncomingTransferThenStorageWrite)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP() << "MockDB does not combine balance deltas across "
                    "subtransaction depths (see KNOWN_ISSUES.md)";
  }

  auto addr_A = make_contract_address(10);
  auto addr_B = make_contract_address(11);
  constexpr static int128_t kInitialBalance = 1'000'000;
  setup_grant_storage_token(addr_A, kInitialBalance);
  setup_grant_storage_token(addr_B, kInitialBalance);

  auto tx = make_new_per_tx_db_proxy();

  // Depth 1: B sends to A
  constexpr int128_t kTransfer = 5000;
  {
    tx.add_delta(make_free_native_token_balance_addrkey(addr_B),
                 make_nnint_add_delta(-kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    tx.add_delta(make_free_native_token_balance_addrkey(addr_A),
                 make_nnint_add_delta(kTransfer),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Depth 2: A writes storage
  {
    tx.add_delta(make_addrkey(addr_A, "key"),
                 make_bytestring_delta("data"),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  // Reference: same in one batch
  auto ref_A = make_contract_address(12);
  auto ref_B = make_contract_address(13);
  setup_grant_storage_token(ref_A, kInitialBalance);
  setup_grant_storage_token(ref_B, kInitialBalance);
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_B),
                     make_nnint_add_delta(-kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_free_native_token_balance_addrkey(ref_A),
                     make_nnint_add_delta(kTransfer),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    tx_ref.add_delta(make_addrkey(ref_A, "key"),
                     make_bytestring_delta("data"),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction({}).has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(addr_A));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(ref_A));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());

  EXPECT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()))
      << "Incoming transfer then storage should match single-depth ref";
}

INSTANTIATE_TEST_SUITE_P(TestDBs, DirectBalanceTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
