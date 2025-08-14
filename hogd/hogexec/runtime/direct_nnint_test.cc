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

class DirectNNIntTest : public ::testing::TestWithParam<TestDB>,
                        public NativeContractTestFramework {
public:
  DirectNNIntTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

  static auto make_addrkey(ContractAddress contract_addr, std::string_view key)
  {
    return make_contract_data_addrkey(contract_addr, key);
  }

  static auto make_nnint_delta(int128_t set, int128_t add)
  {
    StorageDelta d(StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd);
    d.nnint().set_value = to_xdr(set);
    d.nnint().add_value = to_xdr(add);
    return d;
  }

  static auto make_nnint_add_delta(int128_t add) {
    StorageDelta d(StorageDeltaType::kDeltaTypeNonnegativeIntAdd);
    d.nnint_add() = to_xdr(add);
    return d;
  }
};

TEST_P(DirectNNIntTest, MultiPopLeavesBaseDelta)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto key = make_addrkey(contract_addr, "nn_key");

  // tx: +100, then +200, then +300; pop 2 → expect +100 at commit
  auto tx = make_new_per_tx_db_proxy();

  // +100
  {
    tx.add_delta(key, make_nnint_delta(0, 100), std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // +200
  {
    tx.add_delta(key, make_nnint_delta(0, 200), std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // +300
  {
    tx.add_delta(key, make_nnint_delta(0, 300), std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // pop last two subtransactions; keep only the first (+100)
  ASSERT_TRUE(tx.pop_subtransactions(2).has_value());
  ASSERT_TRUE(tx.commit_transaction().has_value());

  advance_block();

  auto res = lookup_storage_item_from_db(key);
  ASSERT_TRUE(res.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_EQ(res->type(), StorageItemType::kItemTypeNonnegativeInt);
  ASSERT_EQ(from_xdr(res->nnint()), 100);
  // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_P(DirectNNIntTest, ComplementaryReservationsAbortPositiveThenCommitNegative)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto key = make_addrkey(contract_addr, "nn_key2");

  // tx1: set_add base=0, +200
  auto tx1 = make_new_per_tx_db_proxy();
  {
    tx1.add_delta(key, make_nnint_delta(0, 200),
                  std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // tx2: add-only -100 (in proposal mode this must fail until +200 commits)
  auto tx2 = make_new_per_tx_db_proxy();
  {
    tx2.add_delta(key, make_nnint_add_delta(-100),
                  std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx2.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_FALSE(r.value());
  }

  // Abort the positive; committing the negative is still a no-op (no reservations held)
  ASSERT_TRUE(tx1.abort_transaction().has_value());
  ASSERT_TRUE(tx2.commit_transaction().has_value());

  advance_block();

  // No item should exist and balance should be unchanged
  auto res = lookup_storage_item_from_db(key);
  EXPECT_FALSE(res.has_value());
  auto bal = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  ASSERT_TRUE(bal.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  EXPECT_EQ(from_xdr(bal->nnint()), kInitialBalance);
  // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_P(DirectNNIntTest, ConcurrentPositiveAddsThenAbortOne)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto key = make_addrkey(contract_addr, "nn_key3");

  // tx1: set_add base=0, +200
  auto tx1 = make_new_per_tx_db_proxy();
  {
    tx1.add_delta(key, make_nnint_delta(0, 200), std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // tx2: set_add base=0, +300
  auto tx2 = make_new_per_tx_db_proxy();
  {
    tx2.add_delta(key, make_nnint_delta(0, 300), std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx2.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Abort tx1; commit tx2
  ASSERT_TRUE(tx1.abort_transaction().has_value());
  ASSERT_TRUE(tx2.commit_transaction().has_value());

  advance_block();

  auto res = lookup_storage_item_from_db(key);
  ASSERT_TRUE(res.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_EQ(res->type(), StorageItemType::kItemTypeNonnegativeInt);
  ASSERT_EQ(from_xdr(res->nnint()), 300);
  // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_P(DirectNNIntTest, SetAddDifferentBaseConflict)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto key = make_addrkey(contract_addr, "nn_key4");

  // tx1: set_add base=0, +100
  auto tx1 = make_new_per_tx_db_proxy();
  {
    tx1.add_delta(key, make_nnint_delta(0, 100),
                  std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // tx2: set_add base=50, +50 (different base should conflict at reservation time)
  auto tx2 = make_new_per_tx_db_proxy();
  {
    tx2.add_delta(key, make_nnint_delta(50, 50),
                  std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx2.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_FALSE(r.value());
  }

  // Clean up: abort both transactions so mockdb closes cleanly
  ASSERT_TRUE(tx2.abort_transaction().has_value());
  ASSERT_TRUE(tx1.abort_transaction().has_value());
  advance_block();
}

TEST_P(DirectNNIntTest, ConcurrentLargeAddsAbortOne)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto key = make_addrkey(contract_addr, "nn_key5");

  // tx1: large +A
  auto tx1 = make_new_per_tx_db_proxy();
  {
    tx1.add_delta(key, make_nnint_delta(0, (int128_t{1} << 60)),
                  std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // tx2: slightly smaller +B
  auto tx2 = make_new_per_tx_db_proxy();
  {
    tx2.add_delta(key, make_nnint_delta(0, (int128_t{1} << 60) - 5),
                  std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx2.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Abort tx2; commit tx1
  ASSERT_TRUE(tx2.abort_transaction().has_value());
  ASSERT_TRUE(tx1.commit_transaction().has_value());

  advance_block();

  auto res = lookup_storage_item_from_db(key);
  ASSERT_TRUE(res.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_EQ(res->type(), StorageItemType::kItemTypeNonnegativeInt);
  ASSERT_EQ(from_xdr(res->nnint()), (int128_t{1} << 60));
  // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_P(DirectNNIntTest, NestedSavepointsMixedAdds)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto key = make_addrkey(contract_addr, "nn_key6");

  // First tx: commit +500 so snapshot base becomes 500
  {
    auto tx_init = make_new_per_tx_db_proxy();
    tx_init.add_delta(key, make_nnint_delta(0, 500),
                      std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx_init.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
    ASSERT_TRUE(tx_init.commit_transaction().has_value());
    advance_block();
  }

  // Second tx: -200 then -400; since only the last delta counts, both reservations may succeed.
  auto tx = make_new_per_tx_db_proxy();

  // -200 (allowed)
  {
    StorageDelta d(StorageDeltaType::kDeltaTypeNonnegativeIntAdd);
    d.nnint_add() = to_xdr(int128_t{-200});
    tx.add_delta(key, d, std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // -400 (updates last delta; allowed relative to base 500)
  {
    StorageDelta d(StorageDeltaType::kDeltaTypeNonnegativeIntAdd);
    d.nnint_add() = to_xdr(int128_t{-400});
    tx.add_delta(key, d, std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  // Pop both subtransactions; commit → final remains 500
  ASSERT_TRUE(tx.pop_subtransactions(2).has_value());
  ASSERT_TRUE(tx.commit_transaction().has_value());

  advance_block();

  auto res = lookup_storage_item_from_db(key);
  ASSERT_TRUE(res.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_EQ(res->type(), StorageItemType::kItemTypeNonnegativeInt);
  ASSERT_EQ(from_xdr(res->nnint()), 500);
  // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_P(DirectNNIntTest, MultiKeyRefundOrdering)
{
  auto contract_addr = make_contract_address(0);
  auto reference_addr = make_contract_address(1);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);
  setup_grant_storage_token(reference_addr, kInitialBalance);

  auto key_a = make_addrkey(contract_addr, "nn_keyA");
  auto key_b = make_addrkey(contract_addr, "nn_keyB");

  // On test address: create A (commit), create B (abort)
  auto tx1 = make_new_per_tx_db_proxy();
  {
    tx1.add_delta(key_a, make_nnint_delta(0, 1),
                  std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  auto tx2 = make_new_per_tx_db_proxy();
  {
    tx2.add_delta(key_b, make_nnint_delta(0, 2),
                  std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx2.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx2.abort_transaction().has_value());
  ASSERT_TRUE(tx1.commit_transaction().has_value());

  // On reference address: only create A
  auto ref_a = make_addrkey(reference_addr, "nn_keyA");
  auto tx_ref = make_new_per_tx_db_proxy();
  {
    tx_ref.add_delta(ref_a, make_nnint_delta(0, 1),
                     std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx_ref.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }
  ASSERT_TRUE(tx_ref.commit_transaction().has_value());

  advance_block();

  auto bal_test = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(contract_addr));
  auto bal_ref = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(reference_addr));
  ASSERT_TRUE(bal_test.has_value());
  ASSERT_TRUE(bal_ref.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_EQ(from_xdr(bal_test->nnint()), from_xdr(bal_ref->nnint()));
  // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_P(DirectNNIntTest, TwoConcurrentAddsBothCommit)
{
  auto contract_addr = make_contract_address(0);
  constexpr static int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(contract_addr, kInitialBalance);

  auto key = make_addrkey(contract_addr, "nn_key7");

  auto tx1 = make_new_per_tx_db_proxy();
  {
    tx1.add_delta(key, make_nnint_delta(0, 150),
                  std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  auto tx2 = make_new_per_tx_db_proxy();
  {
    tx2.add_delta(key, make_nnint_delta(0, 250),
                  std::to_underlying(StorageItemRPCFlags::kStorageNone));
    auto r = tx2.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
  }

  ASSERT_TRUE(tx1.commit_transaction().has_value());
  ASSERT_TRUE(tx2.commit_transaction().has_value());

  advance_block();

  auto res = lookup_storage_item_from_db(key);
  ASSERT_TRUE(res.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_EQ(res->type(), StorageItemType::kItemTypeNonnegativeInt);
  ASSERT_EQ(from_xdr(res->nnint()), 400);
  // NOLINTEND(bugprone-unchecked-optional-access)
}

INSTANTIATE_TEST_SUITE_P(TestDBs, DirectNNIntTest,
                         ::testing::Values(TestDB::kMockInMemory, TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd 