#include <gtest/gtest.h>

#include <string_view>
#include <utility>
#include <vector>

#include "account.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_delta_flags.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/x23.h"

namespace hogd {

// Tests for reference leak bugs in storage item delta management
// These tests verify that when a delta reservation fails, the reference count
// is properly released, allowing subsequent reservations of different types.
class ReferenceLeakTest : public ::testing::TestWithParam<TestDB>,
                          public NativeContractTestFramework {
public:
  ReferenceLeakTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

  static auto make_addrkey(ContractAddress contract_addr, std::string_view key)
  {
    return make_contract_data_addrkey(contract_addr, key);
  }

  static auto make_bytestring_delta(std::string_view bstring)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeBytestringSet);
    delta.bytestring() = make_bytestring(bstring);
    return delta;
  }

  static auto make_nnint_delta(int128_t set, int128_t add)
  {
    StorageDelta d(StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd);
    d.nnint().set_value = to_xdr(set);
    d.nnint().add_value = to_xdr(add);
    return d;
  }

  static auto make_auth_data_delta(std::vector<uint32_t> const &key_ids)
  {
    const auto empty_config =
        PublicKeyConfig{.key = PublicKey{CryptographicScheme::Ed25519},
                        .max_weight = Weight{1}};
    StorageDelta delta(StorageDeltaType::kDeltaTypeAuthDataChange);
    for (auto key_id : key_ids) {
      delta.auth_data_change().auth_data_change_signers.push_back(
          AuthDataChangeSigner{.public_key_id = key_id,
                               .public_key = x23::Ptr<PublicKeyConfig>(
                                   new PublicKeyConfig(empty_config))});
    }
    return delta;
  }

  static auto make_rcq_consume_delta()
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().insert = RetrocausalQueueInsertDelta{};
    delta.retrocausal_queue().consume.emplace(
        RetrocausalQueueConsumeDelta{.past_block_consume =
                                         RetrocausalQueuePastBlockConsumeDelta{
                                             .max_fully_consumed_block_number =
                                                 0,
                                             .partial_consumed_number = 0},
                                     .current_block_consumed = {}});
    return delta;
  }

  static auto make_rcq_insert_only_delta()
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().insert = RetrocausalQueueInsertDelta{};
    // No consume - this delta only inserts
    return delta;
  }

  static auto make_replay_cache_delta(ReplayNonce nonce)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeReplayCacheModify);
    delta.replay_cache_modify().minimum_threshold = 0;
    delta.replay_cache_modify().nonces = {nonce};
    return delta;
  }
};

// Test RCQ reference leak: when try_reserve_consume() fails,
// reference should be released allowing other operations to succeed
TEST_P(ReferenceLeakTest, RCQFailedReservationReleasesReference)
{
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP() << "RCQ is not supported in HogDB";
    return;
  }
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto key = make_addrkey(contract_addr, "test");

  // First, reserve RCQ with consume to set the consumer_lock_
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_rcq_consume_delta(),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value()); // Should succeed

  // Now try to reserve another RCQ delta that also needs consume
  // This should fail because consumer_lock_ is already true
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_rcq_consume_delta(),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_FALSE(r2.value()) << "rcq_conflict: Second consume should fail";

  // Here the reference is held; adding a bytestring here should fail
  {
    auto tx_intermediate = make_new_per_tx_db_proxy();
    tx_intermediate.add_delta(key, make_bytestring_delta("foo"),
                              std::to_underlying(
                                  StorageItemRPCFlags::kStorageNone));
    auto r_intermediate = tx_intermediate.try_push_current_subtransaction();
    ASSERT_TRUE(r_intermediate.has_value());
    ASSERT_FALSE(r_intermediate.value())
        << "bytestring_conflict: Intermediate bytestring should fail";
    ASSERT_TRUE(tx_intermediate.abort_transaction().has_value());
  }

  // Here's where the reference must be released.
  ASSERT_TRUE(tx1.abort_transaction().has_value());

  // The bug test: now try to reserve an RCQ delta that doesn't need consume
  // This should succeed if the reference was properly released on the failure
  // above
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key, make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r3 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value())
      << "Should be able to reserve insert-only RCQ after consume conflict";

  // Clean up
  ASSERT_TRUE(tx2.abort_transaction().has_value());
  ASSERT_TRUE(tx3.commit_transaction().has_value());
}

// Test Auth Data reference leak: when try_reserve_change_signer() fails,
// reference should be released allowing other reservations to succeed
TEST_P(ReferenceLeakTest, AuthDataFailedReservationReleasesReference)
{
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP() << "Auth data is not supported in HogDB";
    return;
  }
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto key = make_addrkey(contract_addr, "test");

  // First, create a successful auth data reservation to occupy key ID 0
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_auth_data_delta({0, 2}),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  // Now try to reserve the same key ID again - this should fail
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_auth_data_delta({1, 2}), // Overlap in key ids
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_FALSE(r2.value())
      << "auth_conflict: Second auth data delta with same key ID should fail";

  // Here the reference is held; adding a bytestring here should fail
  {
    auto tx_intermediate = make_new_per_tx_db_proxy();
    tx_intermediate.add_delta(key, make_bytestring_delta("foo"),
                              std::to_underlying(
                                  StorageItemRPCFlags::kStorageNone));
    auto r_intermediate = tx_intermediate.try_push_current_subtransaction();
    ASSERT_TRUE(r_intermediate.has_value());
    ASSERT_FALSE(r_intermediate.value())
        << "bytestring_conflict: Intermediate bytestring should fail";
    ASSERT_TRUE(tx_intermediate.abort_transaction().has_value());
  }

  // Here's where the reference must be released.
  ASSERT_TRUE(tx1.abort_transaction().has_value());

  // The bug test: now try to reserve a different type after the failed auth
  // reservation This should succeed if the reference was properly released on
  // the failure above
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key, make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r3 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value())
      << "Should be able to reserve bytestring after auth conflict";

  // Clean up
  ASSERT_TRUE(tx2.abort_transaction().has_value());
  ASSERT_TRUE(tx3.commit_transaction().has_value());
}

// Test ReplayCache reference leak: when try_reserve_insert() fails,
// reference should be released allowing other types to be reserved
TEST_P(ReferenceLeakTest, ReplayCacheFailedReservationReleasesReference)
{
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP() << "Replay cache is not supported in HogDB";
    return;
  }
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto key = make_addrkey(contract_addr, "test");

  // First, create a replay cache with nonce 100
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_replay_cache_delta(100),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  // Now try to reserve the same nonce again - this should fail
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_replay_cache_delta(100), // Same nonce
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_FALSE(r2.value()) << "replay_conflict: Duplicate nonce should fail";

  // Here the reference is held; adding a bytestring here should fail
  {
    auto tx_intermediate = make_new_per_tx_db_proxy();
    tx_intermediate.add_delta(key, make_bytestring_delta("foo"),
                              std::to_underlying(
                                  StorageItemRPCFlags::kStorageNone));
    auto r_intermediate = tx_intermediate.try_push_current_subtransaction();
    ASSERT_TRUE(r_intermediate.has_value());
    ASSERT_FALSE(r_intermediate.value())
        << "bytestring_conflict: Intermediate bytestring should fail";
    ASSERT_TRUE(tx_intermediate.abort_transaction().has_value());
  }

  // Here's where the reference must be released.
  ASSERT_TRUE(tx1.abort_transaction().has_value());

  // The bug test: now try to reserve a different type after the failed replay
  // cache reservation This should succeed if the reference was properly
  // released on the failure above
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key, make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r3 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value())
      << "Should be able to reserve bytestring after replay cache conflict";

  // Clean up
  ASSERT_TRUE(tx2.abort_transaction().has_value());
  ASSERT_TRUE(tx3.commit_transaction().has_value());
}

// Test NNInt reference leak: when try_acquire_bounds_lock() fails,
// reference should be released allowing other types to be reserved
TEST_P(ReferenceLeakTest, NNIntFailedReservationReleasesReference)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto key = make_addrkey(contract_addr, "test");

  // Create a scenario where bounds lock acquisition will fail
  // Set a small positive base value first
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_nnint_delta(5, 0), // Set to 5
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  // Now try to subtract more than the base value - this should fail
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(
      key,
      make_nnint_delta(5, -10), // Try to subtract 10 from 5 (would go negative)
      std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_FALSE(r2.value())
      << "nnint_conflict: Subtraction that would go negative should fail";

  // Here the reference is held; adding a bytestring here should fail
  {
    auto tx_intermediate = make_new_per_tx_db_proxy();
    tx_intermediate.add_delta(key, make_bytestring_delta("foo"),
                              std::to_underlying(
                                  StorageItemRPCFlags::kStorageNone));
    auto r_intermediate = tx_intermediate.try_push_current_subtransaction();
    ASSERT_TRUE(r_intermediate.has_value());
    ASSERT_FALSE(r_intermediate.value())
        << "bytestring_conflict: Intermediate bytestring should fail";
    ASSERT_TRUE(tx_intermediate.abort_transaction().has_value());
  }

  // Here's where the reference must be released.
  ASSERT_TRUE(tx1.abort_transaction().has_value());

  // The bug test: now try to reserve a different type after the failed NNInt
  // reservation This should succeed if the reference was properly released on
  // the failure above
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key, make_bytestring_delta("value"),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r3 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value())
      << "Should be able to reserve bytestring after nnint conflict";

  // Clean up
  ASSERT_TRUE(tx2.abort_transaction().has_value());
  ASSERT_TRUE(tx3.commit_transaction().has_value());
}

TEST_P(ReferenceLeakTest, AuthDataMultipleSignersReleaseLocks)
{
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP() << "Auth data is not supported in HogDB";
    return;
  }

  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto key = make_addrkey(contract_addr, "test");

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_auth_data_delta({0, 1, 5}),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r1 = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_auth_data_delta({2, 5, 7}),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r2 = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r2.has_value());
  ASSERT_FALSE(r2.value());

  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key, make_auth_data_delta({2, 7}),
                std::to_underlying(StorageItemRPCFlags::kStorageNone));
  auto r3 = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r3.has_value());
  ASSERT_TRUE(r3.value());

  ASSERT_TRUE(tx1.commit_transaction().has_value());
  ASSERT_TRUE(tx2.abort_transaction().has_value());
  ASSERT_TRUE(tx3.commit_transaction().has_value());
}

INSTANTIATE_TEST_SUITE_P(TestDBs, ReferenceLeakTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
