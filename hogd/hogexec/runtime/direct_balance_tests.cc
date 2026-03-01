#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>
#include <utility>

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

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, DirectBalanceTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
