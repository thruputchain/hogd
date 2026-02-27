#include <gtest/gtest.h>

#include <format>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "address_auth.hh"
#include "auth_card.hh"
#include "auth_claim.hh"
#include "auth_rpc.hh"
#include "hogd/base/io.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/auth_change_subscription_manager.h"
#include "hogd/hogdb/background_commit_worker.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/invoke_tx.h"
#include "hogd/hogdb/mock_authdb_rpc.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/version.h"
#include "storage_item.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/x23.h"

namespace hogd {

namespace {
// Helper to create a PublicKeyConfig
PublicKeyConfigOpt
make_test_pk_config(uint8_t value, Weight weight = 100)
{
  PublicKey key;
  key.ed25519().fill(value);
  return x23::Ptr<PublicKeyConfig>(
      new PublicKeyConfig{.key = key, .max_weight = weight});
}

constexpr StorageCostModel kTestCostModel =
    StorageCostModel(12000, 1, 1, 1, 1234);
} // namespace

class AuthSubscriptionTest : public testing::Test {
protected:
  void SetUp() override
  {
    ErrnoStatus<Directory> dir = Directory::open_temporary();
    ASSERT_TRUE(static_cast<bool>(dir));

    constexpr size_t kHeapSize = 10_MiB;
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), Database::estimate_heap_size(kHeapSize),
                       0);
    ASSERT_TRUE(db);
    db_ = HogDBDAO(RAMDB(std::move(*db)));

    // Start mock auth server
    mock_auth_server_.emplace();
    uint16_t port = mock_auth_server_->start_server(0);

    // Create AuthCard pointing to the mock server
    std::string server_address = std::format("localhost:{}", port);
    auth_card_ = AuthCard{.protocol = version::kHogdProtocol(),
                          .upid = {},
                          .address_for_db = server_address.c_str(),
                          .address_for_mempool = ""};
  }

  void grant_storage_token(Address const &address, int128_t amount)
  {
    auto ret = db_.write_address_balance(address, amount);
    ASSERT_TRUE(ret) << "grant_storage_token: write_address_balance failed";
  }

  HogDBDAO db_;
  MerkleTriePrefix trie_;
  std::optional<MockAuthDBServer> mock_auth_server_;
  AuthCard auth_card_;
  AuthChangeSubscriptionManager subscription_manager_;
};
// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_F(AuthSubscriptionTest, AuthChangesAreForwardedToSubscriber)
{
  // Subscribe to auth changes
  subscription_manager_.add_full_auth_change_subscriber(auth_card_)();

  // Create 3 addresses
  Address addr1 = make_address(0x1111);
  Address addr2 = make_address(0x2222);
  Address addr3 = make_address(0x3333);

  // Grant storage tokens to each address
  constexpr int128_t kStorageTokens = 1'000'000;
  grant_storage_token(addr1, kStorageTokens);
  grant_storage_token(addr2, kStorageTokens);
  grant_storage_token(addr3, kStorageTokens);

  BlockHeight height = 1;
  db_.load_trie(trie_);

  // Create PerThreadARC<BlockTX> and InvokeTX
  BackgroundCommitWorker background_commit_worker(subscription_manager_,
                                                  nullptr);
  PerThreadARC<BlockTX> block_tx(db_, trie_, kTestCostModel,
                                 background_commit_worker, height);
  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);

  // Create 3 auth data changes using InvokeTX API
  AuthDataChangeArgs auth_data1{};
  auth_data1.auth_data_change_signers.push_back(
      AuthDataChangeSigner{.public_key_id = 0,
                           .public_key = make_test_pk_config(1)});

  AuthDataChangeArgs auth_data2{};
  auth_data2.auth_data_change_signers.push_back(
      AuthDataChangeSigner{.public_key_id = 1,
                           .public_key = make_test_pk_config(2)});

  AuthDataChangeArgs auth_data3{};
  auth_data3.auth_data_change_signers.push_back(
      AuthDataChangeSigner{.public_key_id = 2,
                           .public_key = make_test_pk_config(3)});

  // Request auth data changes
  HogDBStatus<void> ret1 = invoke_tx.request_auth_data(
      addr1, auth_data1,
      std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  ASSERT_TRUE(ret1);
  HogDBStatus<void> ret2 = invoke_tx.request_auth_data(
      addr2, auth_data2,
      std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  ASSERT_TRUE(ret2);
  HogDBStatus<void> ret3 = invoke_tx.request_auth_data(
      addr3, auth_data3,
      std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  ASSERT_TRUE(ret3);

  HogDBStatus<void> ret = invoke_tx.sync_balance_locks();
  ASSERT_TRUE(ret);

  // Commit InvokeTX (applies changes to BlockTX)
  invoke_tx.commit();
  invoke_tx.release_blocktx_reference();

  // Commit BlockTX (writes to database and publishes auth changes)
  HogDBStatus<void> commit_result = block_tx.get().commit();
  ASSERT_TRUE(commit_result);

  // Wait for changes to be received
  mock_auth_server_->impl()->wait_for_changes(height);

  // Verify the changes were received
  auto received = mock_auth_server_->impl()->get_changes_at_height(height);
  ASSERT_TRUE(received.has_value());
  EXPECT_EQ(received->size(), 3);

  // Verify each address is present
  std::set<ContractAddress> received_addresses;
  for (const auto &change : *received) {
    received_addresses.insert(change.address);
  }
  EXPECT_TRUE(received_addresses.contains(addr1.get_raw()));
  EXPECT_TRUE(received_addresses.contains(addr2.get_raw()));
  EXPECT_TRUE(received_addresses.contains(addr3.get_raw()));

  // Verify auth data is correct
  for (const auto &change : *received) {
    ASSERT_NE(change.auth_data, nullptr);
    if (change.address == addr1.get_raw()) {
      EXPECT_NE(change.auth_data->public_keys[0], nullptr);
    }
    else if (change.address == addr2.get_raw()) {
      EXPECT_NE(change.auth_data->public_keys[1], nullptr);
    }
    else if (change.address == addr3.get_raw()) {
      EXPECT_NE(change.auth_data->public_keys[2], nullptr);
    }
    else {
      ADD_FAILURE() << "Unexpected address";
    }
  }
}

TEST_F(AuthSubscriptionTest, AuthDeletionsAreForwardedToSubscriber)
{
  // Subscribe to auth changes
  subscription_manager_.add_full_auth_change_subscriber(auth_card_)();

  // Create an address
  Address addr = make_address(0x4444);

  // Grant storage tokens to the address
  constexpr int128_t kStorageTokens = 1'000'000;
  grant_storage_token(addr, kStorageTokens);

  // Write auth data directly to the database
  AddressAuthData initial_auth_data{};
  initial_auth_data.public_keys[0] = make_test_pk_config(42);
  HogDBStatus<void> write_ret =
      db_.write_address_auth_data(addr, initial_auth_data);
  ASSERT_TRUE(write_ret) << "write_address_auth_data failed";

  BlockHeight height = 1;
  db_.load_trie(trie_);

  // Create PerThreadARC<BlockTX> and InvokeTX
  BackgroundCommitWorker background_commit_worker(subscription_manager_,
                                                  nullptr);
  PerThreadARC<BlockTX> block_tx(db_, trie_, kTestCostModel,
                                 background_commit_worker, height);
  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);

  // Request deletion of the auth data
  ItemKey auth_key = make_address_auth_data_item_key(addr);
  HogDBStatus<void> delete_ret = invoke_tx.request_deletion(
      auth_key, std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  ASSERT_TRUE(delete_ret) << "request_deletion failed";

  HogDBStatus<void> ret = invoke_tx.sync_balance_locks();
  ASSERT_TRUE(ret);

  // Commit InvokeTX (applies changes to BlockTX)
  invoke_tx.commit();
  invoke_tx.release_blocktx_reference();

  // Commit BlockTX (writes to database and publishes auth changes)
  HogDBStatus<void> commit_result = block_tx.get().commit();
  ASSERT_TRUE(commit_result);

  // Wait for changes to be received
  mock_auth_server_->impl()->wait_for_changes(height);

  // Verify the deletion was received
  auto received = mock_auth_server_->impl()->get_changes_at_height(height);
  ASSERT_TRUE(received.has_value());
  EXPECT_EQ(received->size(), 1);

  // Verify the deletion is present (auth_data should be nullptr for deletions)
  EXPECT_EQ(received->at(0).address, addr.get_raw());
  EXPECT_EQ(received->at(0).auth_data, nullptr);
}

TEST_F(AuthSubscriptionTest, AuthChangesHandledGracefullyWhenServerShutsDown)
{
  // Subscribe to auth changes
  subscription_manager_.add_full_auth_change_subscriber(auth_card_)();

  {
    // Create another auth server that is then deleted
    MockAuthDBServer mock_auth_server2;
    uint16_t port2 = mock_auth_server2.start_server(0);
    std::string server_address2 = std::format("localhost:{}", port2);
    AuthCard auth_card2 = AuthCard{.protocol = version::kHogdProtocol(),
                                   .upid = {},
                                   .address_for_db = server_address2.c_str(),
                                   .address_for_mempool = ""};
    subscription_manager_.add_full_auth_change_subscriber(auth_card2)();
  }

  // create one auth server that does receive the updates
  MockAuthDBServer mock_auth_server3;
  uint16_t port3 = mock_auth_server3.start_server(0);
  std::string server_address3 = std::format("localhost:{}", port3);
  AuthCard auth_card3 = AuthCard{.protocol = version::kHogdProtocol(),
                                 .upid = {},
                                 .address_for_db = server_address3.c_str(),
                                 .address_for_mempool = ""};
  subscription_manager_.add_full_auth_change_subscriber(auth_card3)();

  // Create an address
  Address addr = make_address(0x5555);

  // Grant storage tokens to the address
  constexpr int128_t kStorageTokens = 1'000'000;
  grant_storage_token(addr, kStorageTokens);

  BlockHeight height = 1;
  db_.load_trie(trie_);

  // Create PerThreadARC<BlockTX> and InvokeTX
  BackgroundCommitWorker background_commit_worker(subscription_manager_,
                                                  nullptr);
  PerThreadARC<BlockTX> block_tx(db_, trie_, kTestCostModel,
                                 background_commit_worker, height);
  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);

  // Create auth data change
  AuthDataChangeArgs auth_data{};
  auth_data.auth_data_change_signers.push_back(
      AuthDataChangeSigner{.public_key_id = 0,
                           .public_key = make_test_pk_config(99)});

  // Request auth data change
  HogDBStatus<void> ret = invoke_tx.request_auth_data(
      addr, auth_data,
      std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  ASSERT_TRUE(ret);

  HogDBStatus<void> sync_ret = invoke_tx.sync_balance_locks();
  ASSERT_TRUE(sync_ret);

  // Commit InvokeTX (applies changes to BlockTX)
  invoke_tx.commit();
  invoke_tx.release_blocktx_reference();

  // Shut down the mock server before committing BlockTX
  // This simulates the server going down before auth changes are sent
  mock_auth_server_.reset();

  // Commit BlockTX (should succeed even though auth server is down)
  // The subscription manager should handle the error gracefully
  HogDBStatus<void> commit_result = block_tx.get().commit();
  ASSERT_TRUE(commit_result)
      << "BlockTX commit should succeed even if auth server is down";

  mock_auth_server3.impl()->wait_for_changes(height);
  auto received3 = mock_auth_server3.impl()->get_changes_at_height(height);
  ASSERT_TRUE(received3.has_value());
  EXPECT_EQ(received3->size(), 1);
  EXPECT_EQ(received3->at(0).address, addr.get_raw());
  EXPECT_NE(received3->at(0).auth_data, nullptr);
}

TEST_F(AuthSubscriptionTest, ErrorBadAuthCard)
{
  AuthCard bad_card = AuthCard{.protocol = version::kHogdProtocol(),
                               .upid = {},
                               .address_for_db = "localhost:12345",
                               .address_for_mempool = ""};
  AuthCard bad_card2 = AuthCard{.protocol = version::kHogdProtocol(),
                                .upid = {},
                                .address_for_db = "unresolvable:12345",
                                .address_for_mempool = ""};
  AuthCard bad_card3 = AuthCard{.protocol = version::kHogdProtocol(),
                                .upid = {},
                                .address_for_db = "unparseable_nonsense",
                                .address_for_mempool = ""};
  // Subscribe to auth changes
  subscription_manager_.add_full_auth_change_subscriber(bad_card)();
  subscription_manager_.add_full_auth_change_subscriber(bad_card2)();
  subscription_manager_.add_full_auth_change_subscriber(bad_card3)();
  subscription_manager_.add_full_auth_change_subscriber(auth_card_)();

  // Create an address
  Address addr = make_address(0x5555);

  // Grant storage tokens to the address
  constexpr int128_t kStorageTokens = 1'000'000;
  grant_storage_token(addr, kStorageTokens);

  BlockHeight height = 1;
  db_.load_trie(trie_);

  // Create PerThreadARC<BlockTX> and InvokeTX
  BackgroundCommitWorker background_commit_worker(subscription_manager_,
                                                  nullptr);

  PerThreadARC<BlockTX> block_tx(db_, trie_, kTestCostModel,
                                 background_commit_worker, height);
  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);

  // Create auth data change
  AuthDataChangeArgs auth_data{};
  auth_data.auth_data_change_signers.push_back(
      AuthDataChangeSigner{.public_key_id = 0,
                           .public_key = make_test_pk_config(99)});

  // Request auth data change
  HogDBStatus<void> ret = invoke_tx.request_auth_data(
      addr, auth_data,
      std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  ASSERT_TRUE(ret);

  HogDBStatus<void> sync_ret = invoke_tx.sync_balance_locks();
  ASSERT_TRUE(sync_ret);

  // Commit InvokeTX (applies changes to BlockTX)
  invoke_tx.commit();
  invoke_tx.release_blocktx_reference();

  // Commit BlockTX (writes to database and publishes auth changes)
  HogDBStatus<void> commit_result = block_tx.get().commit();
  ASSERT_TRUE(commit_result);

  // Verify that the good server still received the change
  mock_auth_server_->impl()->wait_for_changes(height);
  auto received = mock_auth_server_->impl()->get_changes_at_height(height);
  ASSERT_TRUE(received.has_value());
  EXPECT_EQ(received->size(), 1);
  EXPECT_EQ(received->at(0).address, addr.get_raw());
  EXPECT_NE(received->at(0).auth_data, nullptr);
}
// NOLINTEND(bugprone-unchecked-optional-access)
} // namespace hogd
