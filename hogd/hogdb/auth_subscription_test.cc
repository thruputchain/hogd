#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include "address_auth.hh"
#include "auth_card.hh"
#include "auth_claim.hh"
#include "auth_rpc.hh"
#include "consensus.hh"
#include "db_sched.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/io.h"
#include "hogd/base/perthread.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/auth_change_subscription_manager.h"
#include "hogd/hogdb/background_commit_worker.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/invoke_tx.h"
#include "hogd/hogdb/mock_authdb_rpc.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_auth.h"
#include "hogd/hogdb/rpc_from_sched.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/version.h"
#include "rpc_types.hh"
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

  // Reproduces the scheduler commit window from issue #1725: an auth-data
  // change is applied to the live DAO by BlockTX::commit() at height
  // kNextHeight, but the committed tip is deliberately NOT advanced (no
  // update_dbtip), so current_committed_height_ stays at kAcceptedHeight. After
  // this returns, ctx_opt_ holds a context whose DAO already has the
  // kNextHeight auth row for raced_addr_ while reporting kAcceptedHeight as the
  // committed height.
  static constexpr BlockHeight kAcceptedHeight = 0;
  static constexpr BlockHeight kNextHeight = kAcceptedHeight + 1;

  void commit_raced_auth_change()
  {
    constexpr BlockTXID kBlockId = 1;
    constexpr InvokeTXID kInvokeId = 1;
    raced_addr_.emplace(make_address(0x6666));
    const Address &addr = *raced_addr_;

    AddressAuthData old_auth_data{};
    old_auth_data.public_keys[0] = make_test_pk_config(1);
    old_auth_data.required_weight = Weight{1};
    old_auth_data.last_written_height = kAcceptedHeight;
    ASSERT_TRUE(db_.write_address_auth_data(addr, old_auth_data));
    grant_storage_token(addr, 1'000'000);

    ConsensusValue tip_value{};
    tip_value.proposal_header.block_height = kAcceptedHeight;
    DbTip tip{.id = 0, .storage_long_hash = {}, .value = tip_value};
    ASSERT_TRUE(db_.write_dbtip(tip));

    ctx_opt_.emplace(std::move(db_), nullptr);
    RPCContext &ctx = *ctx_opt_;
    ctx.initialize_cost_model(kTestCostModel);
    ASSERT_EQ(ctx.get_height_threadsafe(), kAcceptedHeight);

    ASSERT_TRUE(
        ctx.open_block_tx(kBlockId,
                          BlockExecutionMode::kBlockExecutionModeProposal));
    InvokeTX *invoke_tx = ctx.get_invoke_tx(kBlockId, kInvokeId);
    ASSERT_NE(invoke_tx, nullptr);

    AuthDataChangeArgs auth_change{};
    auth_change.auth_data_change_signers.push_back(
        AuthDataChangeSigner{.public_key_id = 0,
                             .public_key = make_test_pk_config(2)});
    auth_change.new_required_weight = Weight{7};

    ASSERT_TRUE(
        invoke_tx
            ->request_auth_data(addr, auth_change,
                                std::to_underlying(StorageDeltaFlags::None))
            .has_value());
    ASSERT_TRUE(invoke_tx->sync_balance_locks().has_value());
    ASSERT_TRUE(invoke_tx->commit().has_value());
    ctx.close_invoke_tx(kBlockId, kInvokeId, invoke_tx);

    PerThreadARC<BlockTX> *block_tx = ctx.get_block_tx(kBlockId, false);
    ASSERT_NE(block_tx, nullptr);
    ASSERT_EQ(block_tx->get().get_height(), kNextHeight);
    HogDBStatus<StorageLongHash> db_hash =
        block_tx->get().close(ctx.get_tip().storage_long_hash);
    ASSERT_TRUE(db_hash.has_value());
    ASSERT_TRUE(block_tx->get().commit().has_value());

    // Tip intentionally NOT advanced: the DAO now holds the kNextHeight auth
    // row while the committed height is still kAcceptedHeight.
    ASSERT_EQ(ctx.get_height_threadsafe(), kAcceptedHeight);

    ASSERT_TRUE(block_tx->initiate_shutdown());
    ctx.remove_shutdown_block_tx(kBlockId);
  }

  HogDBDAO db_;
  std::optional<RPCContext> ctx_opt_;
  std::optional<Address> raced_addr_;
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

  // Create PerThreadARC<BlockTX> and InvokeTX
  BackgroundCommitWorker background_commit_worker(subscription_manager_,
                                                  nullptr);
  PerThreadARC<BlockTX> block_tx(
      db_, kTestCostModel, background_commit_worker, height,
      BlockExecutionMode::kBlockExecutionModeProposal);
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
  HogDBStatus<void> ret1 =
      invoke_tx.request_auth_data(addr1, auth_data1,
                                  std::to_underlying(StorageDeltaFlags::None));
  ASSERT_TRUE(ret1);
  HogDBStatus<void> ret2 =
      invoke_tx.request_auth_data(addr2, auth_data2,
                                  std::to_underlying(StorageDeltaFlags::None));
  ASSERT_TRUE(ret2);
  HogDBStatus<void> ret3 =
      invoke_tx.request_auth_data(addr3, auth_data3,
                                  std::to_underlying(StorageDeltaFlags::None));
  ASSERT_TRUE(ret3);

  HogDBStatus<void> ret = invoke_tx.sync_balance_locks();
  ASSERT_TRUE(ret);

  // Commit InvokeTX (applies changes to BlockTX)
  ASSERT_TRUE(invoke_tx.commit());

  // Close and commit BlockTX (writes to database and publishes auth changes)
  ASSERT_TRUE(block_tx.get().close(StorageLongHash{}));
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

  // Create PerThreadARC<BlockTX> and InvokeTX
  BackgroundCommitWorker background_commit_worker(subscription_manager_,
                                                  nullptr);
  PerThreadARC<BlockTX> block_tx(
      db_, kTestCostModel, background_commit_worker, height,
      BlockExecutionMode::kBlockExecutionModeProposal);
  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);

  // Request deletion of the auth data
  ItemKey auth_key = make_address_auth_data_item_key(addr);
  HogDBStatus<void> delete_ret =
      invoke_tx.request_deletion(auth_key,
                                 std::to_underlying(StorageDeltaFlags::None));
  ASSERT_TRUE(delete_ret) << "request_deletion failed";

  HogDBStatus<void> ret = invoke_tx.sync_balance_locks();
  ASSERT_TRUE(ret);

  // Commit InvokeTX (applies changes to BlockTX)
  ASSERT_TRUE(invoke_tx.commit());

  // Close and commit BlockTX (writes to database and publishes auth changes)
  ASSERT_TRUE(block_tx.get().close(StorageLongHash{}));
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

  // Create PerThreadARC<BlockTX> and InvokeTX
  BackgroundCommitWorker background_commit_worker(subscription_manager_,
                                                  nullptr);
  PerThreadARC<BlockTX> block_tx(
      db_, kTestCostModel, background_commit_worker, height,
      BlockExecutionMode::kBlockExecutionModeProposal);
  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);

  // Create auth data change
  AuthDataChangeArgs auth_data{};
  auth_data.auth_data_change_signers.push_back(
      AuthDataChangeSigner{.public_key_id = 0,
                           .public_key = make_test_pk_config(99)});

  // Request auth data change
  HogDBStatus<void> ret =
      invoke_tx.request_auth_data(addr, auth_data,
                                  std::to_underlying(StorageDeltaFlags::None));
  ASSERT_TRUE(ret);

  HogDBStatus<void> sync_ret = invoke_tx.sync_balance_locks();
  ASSERT_TRUE(sync_ret);

  // Commit InvokeTX (applies changes to BlockTX)
  ASSERT_TRUE(invoke_tx.commit());

  // Shut down the mock server before committing BlockTX
  // This simulates the server going down before auth changes are sent
  mock_auth_server_.reset();

  // Close and commit BlockTX (should succeed even though auth server is down)
  // The subscription manager should handle the error gracefully
  ASSERT_TRUE(block_tx.get().close(StorageLongHash{}));
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

  // Create PerThreadARC<BlockTX> and InvokeTX
  BackgroundCommitWorker background_commit_worker(subscription_manager_,
                                                  nullptr);

  PerThreadARC<BlockTX> block_tx(
      db_, kTestCostModel, background_commit_worker, height,
      BlockExecutionMode::kBlockExecutionModeProposal);
  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);

  // Create auth data change
  AuthDataChangeArgs auth_data{};
  auth_data.auth_data_change_signers.push_back(
      AuthDataChangeSigner{.public_key_id = 0,
                           .public_key = make_test_pk_config(99)});

  // Request auth data change
  HogDBStatus<void> ret =
      invoke_tx.request_auth_data(addr, auth_data,
                                  std::to_underlying(StorageDeltaFlags::None));
  ASSERT_TRUE(ret);

  HogDBStatus<void> sync_ret = invoke_tx.sync_balance_locks();
  ASSERT_TRUE(sync_ret);

  // Commit InvokeTX (applies changes to BlockTX)
  ASSERT_TRUE(invoke_tx.commit());

  // Close and commit BlockTX (writes to database and publishes auth changes)
  ASSERT_TRUE(block_tx.get().close(StorageLongHash{}));
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

namespace {

// Mirrors AuthServer's strict ordered-height gate: refuses to complete
// authdb_forward_key_changes(H) until H-1 has been processed. Used to
// demonstrate that concurrent publishes must not be serialized on the
// client side while one is parked on the server-side gate.
class HeightOrderedMockAuthDBRpc {
public:
  using rpc_service = AuthDB1;

  x23::Async<void> authdb_forward_key_changes(BlockHeight height,
                                              KeyChangeBatch /*key_changes*/)
  {
    active_calls_.fetch_add(1, std::memory_order_release);
    arrivals_.fetch_add(1, std::memory_order_release);
    last_arrived_height_.store(height, std::memory_order_release);

    while (processed_height_.load(std::memory_order_acquire) + 1 != height) {
      if (shutdown_.load(std::memory_order_acquire)) {
        active_calls_.fetch_sub(1, std::memory_order_release);
        co_return;
      }
      co_await default_threadpool().bg();
    }

    processed_height_.store(height, std::memory_order_release);
    completed_.fetch_add(1, std::memory_order_release);
    active_calls_.fetch_sub(1, std::memory_order_release);
  }

  int arrivals() const { return arrivals_.load(std::memory_order_acquire); }
  int completed() const { return completed_.load(std::memory_order_acquire); }
  int active_calls() const
  {
    return active_calls_.load(std::memory_order_acquire);
  }
  BlockHeight last_arrived_height() const
  {
    return last_arrived_height_.load(std::memory_order_acquire);
  }
  void shutdown() { shutdown_.store(true, std::memory_order_release); }

private:
  std::atomic<BlockHeight> processed_height_{0};
  std::atomic<BlockHeight> last_arrived_height_{0};
  std::atomic<int> arrivals_{0};
  std::atomic<int> completed_{0};
  std::atomic<int> active_calls_{0};
  std::atomic<bool> shutdown_{false};
};

static_assert(is_AuthDB1<HeightOrderedMockAuthDBRpc *, x23::MaybeAsync>);

class HeightOrderedMockAuthDBServer
  : public RpcServer<HeightOrderedMockAuthDBRpc> {
public:
  HeightOrderedMockAuthDBServer() : RpcServer(default_coropoll()) {}
};

} // namespace

TEST_F(AuthSubscriptionTest, OutOfOrderAuthPublishProgresses)
{
  HeightOrderedMockAuthDBServer mock_server;
  uint16_t port = mock_server.start_server(0);
  std::string server_address = std::format("localhost:{}", port);
  AuthCard ordered_auth_card =
      AuthCard{.protocol = version::kHogdProtocol(),
               .upid = {},
               .address_for_db = server_address.c_str(),
               .address_for_mempool = ""};
  ASSERT_TRUE(subscription_manager_.add_full_auth_change_subscriber(
      ordered_auth_card)());

  // Publish height 2 first. The mock will park it on its ordered-height gate
  // (since processed_height_ is still 0).
  {
    PerThread<KeyChangeBatch> key_changes;
    subscription_manager_.detached_publish_auth_changes(2,
                                                        std::move(key_changes));
  }

  auto arrive_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (mock_server.impl()->arrivals() < 1) {
    ASSERT_LT(std::chrono::steady_clock::now(), arrive_deadline)
        << "height 2 never reached the auth server";
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(mock_server.impl()->last_arrived_height(), 2);

  // Now publish height 1. Before the fix, height 1 cannot enter
  // publish_auth_changes because height 2 holds lock_ while parked on the
  // server's gate, and the test will time out below.
  {
    PerThread<KeyChangeBatch> key_changes;
    subscription_manager_.detached_publish_auth_changes(1,
                                                        std::move(key_changes));
  }

  auto complete_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (mock_server.impl()->completed() < 2) {
    ASSERT_LT(std::chrono::steady_clock::now(), complete_deadline)
        << "auth publishes deadlocked: arrivals="
        << mock_server.impl()->arrivals()
        << " completed=" << mock_server.impl()->completed();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(mock_server.impl()->arrivals(), 2);
  EXPECT_EQ(mock_server.impl()->completed(), 2);

  // Post-condition safety net: flush any lingering coroutines (none expected
  // after completed == 2) before the server tears down.
  mock_server.impl()->shutdown();
  auto cleanup_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (mock_server.impl()->active_calls() != 0) {
    ASSERT_LT(std::chrono::steady_clock::now(), cleanup_deadline)
        << "timed out waiting for detached publishers to exit during cleanup";
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

TEST_F(AuthSubscriptionTest, MalformedProtocolAuthCardIsRejected)
{
  AuthCard empty_protocol_card =
      AuthCard{.protocol = ProtocolCommit{},
               .upid = {},
               .address_for_db = auth_card_.address_for_db,
               .address_for_mempool = ""};

  AuthCard wrong_protocol_card =
      AuthCard{.protocol = "wrong/000000000000",
               .upid = {},
               .address_for_db = auth_card_.address_for_db,
               .address_for_mempool = ""};

  std::string nul_suffixed(version::kHogdProtocol());
  nul_suffixed.push_back('\0');
  nul_suffixed += "suffix";
  AuthCard nul_protocol_card =
      AuthCard{.protocol =
                   ProtocolCommit(nul_suffixed.data(), nul_suffixed.size()),
               .upid = {},
               .address_for_db = auth_card_.address_for_db,
               .address_for_mempool = ""};

  RPCContext ctx(std::move(db_), nullptr);
  HogDBAuthRpc rpc(ctx);

  DBAuthIntroResult empty_result = rpc.dbauth_intro(empty_protocol_card)();
  EXPECT_EQ(empty_result.outcome(), DBAuthIntroOutcome::kIntroFailure);

  DBAuthIntroResult wrong_result = rpc.dbauth_intro(wrong_protocol_card)();
  EXPECT_EQ(wrong_result.outcome(), DBAuthIntroOutcome::kIntroFailure);

  DBAuthIntroResult nul_result = rpc.dbauth_intro(nul_protocol_card)();
  EXPECT_EQ(nul_result.outcome(), DBAuthIntroOutcome::kIntroFailure);

  DBAuthIntroResult valid_result = rpc.dbauth_intro(auth_card_)();
  EXPECT_EQ(valid_result.outcome(), DBAuthIntroOutcome::kIntroSuccess);
}

// Regression test for issue #2814. Reproduces the subscribe-vs-commit window
// deterministically: a block's auth-change publish for height H is dispatched
// (capturing the current subscriber set under lock_) while the committed tip
// still reads H-1, and a new subscriber is added in that window. The new
// subscriber therefore misses height H's publish; it is never re-sent. If the
// intro reports only the committed height H-1, the auth server inits to H-1 and
// then parks forever waiting for H once a later height streams in. The intro
// must instead report a height that covers any already-dispatched publish, so
// the missed height is dropped as stale rather than awaited.
TEST_F(AuthSubscriptionTest, IntroHeightCoversAlreadyPublishedHeight)
{
  constexpr BlockHeight kCommittedHeight = 4;
  constexpr BlockHeight kPublishedHeight = kCommittedHeight + 1;

  // The committed tip is H-1: this is what the racing intro reads.
  ConsensusValue tip_value{};
  tip_value.proposal_header.block_height = kCommittedHeight;
  ASSERT_TRUE(db_.write_dbtip(
      DbTip{.id = 0, .storage_long_hash = {}, .value = tip_value}));
  RPCContext ctx(std::move(db_), nullptr);
  ASSERT_EQ(ctx.get_height_threadsafe(), kCommittedHeight);

  // Height H's publish is dispatched before the subscriber is added — the
  // worst case of the race, forced deterministically. Running it synchronously
  // guarantees start_forward_key_changes has captured the (empty) subscriber
  // set, so the to-be-added subscriber has provably missed height H.
  {
    PerThread<KeyChangeBatch> key_changes;
    ctx.get_auth_change_subscription_manager().publish_auth_changes(
        kPublishedHeight, std::move(key_changes))();
  }

  HogDBAuthRpc rpc(ctx);
  DBAuthIntroResult result = rpc.dbauth_intro(auth_card_)();
  ASSERT_EQ(result.outcome(), DBAuthIntroOutcome::kIntroSuccess);
  // Before the fix this is kCommittedHeight (4) — the wedge. After the fix it
  // covers the dispatched publish (>= 5), so height 5 is dropped as stale.
  EXPECT_GE(result.current_height(), kPublishedHeight);
}

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// Regression test for issue #1725. A committed auth row must carry the height
// of the block that wrote it (write-side stamp), and authdb_get_auth_data must
// not leak a row written by a height newer than the accepted query height.
TEST_F(AuthSubscriptionTest, AuthRpcRejectsFutureAuthRowBeforeTipUpdate)
{
  commit_raced_auth_change();
  RPCContext &ctx = *ctx_opt_;

  // Write-side: the committed row is stamped with the height that wrote it.
  HogDBStatus<AddressAuthData> committed =
      ctx.get_db().read_address_auth_data(*raced_addr_);
  ASSERT_TRUE(committed.has_value());
  ASSERT_EQ(committed->required_weight, Weight{7});
  EXPECT_EQ(committed->last_written_height, kNextHeight);

  // Read-side: the query is accepted for kAcceptedHeight, but the live row was
  // written by kNextHeight. It must reject as stale rather than return the
  // not-yet-published row.
  HogDBAuthRpc auth_rpc(ctx);
  DBAuthResult result =
      auth_rpc.authdb_get_auth_data(kAcceptedHeight, raced_addr_->get_raw())();
  EXPECT_EQ(result.error(), DBAuthError::kDBAuthStaleBlockHeight);
}

// Batch variant of the #1725 regression test.
TEST_F(AuthSubscriptionTest, AuthRpcBatchRejectsFutureAuthRowBeforeTipUpdate)
{
  commit_raced_auth_change();
  RPCContext &ctx = *ctx_opt_;

  HogDBAuthRpc auth_rpc(ctx);
  ContractAddressBatch addresses;
  addresses.push_back(raced_addr_->get_raw());
  DBAuthResultBatch result =
      auth_rpc.authdb_get_auth_data_batch(kAcceptedHeight, addresses)();
  ASSERT_EQ(result.size(), 1U);
  EXPECT_EQ(result[0].error(), DBAuthError::kDBAuthStaleBlockHeight);
}

// The scheduler's dbsched_get_address_auth_data validates against the committed
// block id, but read_dbtip lags the DAO apply the same way the height check
// does. It must also reject a row written by a height newer than the committed
// tip.
TEST_F(AuthSubscriptionTest, SchedRpcRejectsFutureAuthRowBeforeTipUpdate)
{
  commit_raced_auth_change();
  RPCContext &ctx = *ctx_opt_;

  HogDBSchedRpc sched_rpc(ctx);
  ContractAddressBatch addresses;
  addresses.push_back(raced_addr_->get_raw());
  // block_id 0 is still the committed tip, so the id guard passes, but the auth
  // row was written by height 1.
  DbAddressAuthDataBatchResult result =
      sched_rpc.dbsched_get_address_auth_data(0, addresses)();
  EXPECT_EQ(result.result(), DbSchedResult::DbSchedResultError);
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
