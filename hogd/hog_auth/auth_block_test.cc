#include <gtest/gtest.h>

#include <atomic>
#include <format>
#include <utility>

#include "hogd/base/coropoll.h"
#include "hogd/base/rpc_client.h"
#include "hogd/hog_auth/auth_server_test_framework.h"
#include "x23/corosync.h"

namespace hogd {
namespace {

class AuthBlockTest : public ::testing::Test, public AuthServerTestFramework {};

TEST_F(AuthBlockTest, LifecycleUsesBlockIDsAndRejectsInvalidTransitions)
{
  auto &auth = auth_server();
  EXPECT_EQ(auth.open_block(100, 101), RPCError::kRPCBadBlockID);
  auth.register_auth_data_updates(DBAuthTip{.block_id = 100, .height = 0},
                                  {})();
  EXPECT_EQ(auth.open_block(0, 101), RPCError::kRPCBadBlockID);
  ASSERT_EQ(auth.open_block(100, 101), RPCError::kRPCNone);
  EXPECT_EQ(auth.open_block(100, 101), RPCError::kRPCBadBlockID);
  EXPECT_EQ(auth.open_block(101, 102), RPCError::kRPCBadBlockID);
  EXPECT_EQ(auth.commit_block(101)(), RPCError::kRPCBadBlockID);
  EXPECT_EQ(auth.verify_batch(101, {})(), AuthErrorCode::kAuthSuccess);
  EXPECT_EQ(auth.close_block(101)(), RPCError::kRPCNone);
  EXPECT_EQ(auth.verify_batch(101, {})(), AuthErrorCode::kAuthBadBlockID);
  EXPECT_EQ(auth.close_block(101)(), RPCError::kRPCBadBlockID);
  EXPECT_EQ(auth.abandon_block(101)(), RPCError::kRPCNone);
  EXPECT_EQ(auth.abandon_block(101)(), RPCError::kRPCBadBlockID);
  EXPECT_EQ(auth.commit_block(101)(), RPCError::kRPCBadBlockID);
}

TEST_F(AuthBlockTest, CommitWaitsForTheSelectedBlockAndRejectsItsSibling)
{
  auto &auth = auth_server();
  auth.register_auth_data_updates(DBAuthTip{.block_id = 100, .height = 0},
                                  {})();
  ASSERT_EQ(auth.open_block(100, 101), RPCError::kRPCNone);
  ASSERT_EQ(auth.open_block(100, 102), RPCError::kRPCNone);
  ASSERT_EQ(auth.close_block(101)(), RPCError::kRPCNone);
  ASSERT_EQ(auth.close_block(102)(), RPCError::kRPCNone);
  auto selected = auth.commit_block(101);
  auto sibling = auth.commit_block(102);
  EXPECT_FALSE(selected.operator co_await().await_ready());
  EXPECT_FALSE(sibling.operator co_await().await_ready());
  auth.register_auth_data_updates(DBAuthTip{.block_id = 101, .height = 1},
                                  {})();
  EXPECT_EQ(selected(), RPCError::kRPCNone);
  EXPECT_EQ(sibling(), RPCError::kRPCBadBlockID);
  EXPECT_EQ(auth.open_block(101, 103), RPCError::kRPCNone);
  EXPECT_EQ(auth.abandon_block(103)(), RPCError::kRPCNone);
}

TEST_F(AuthBlockTest, LateCommitAndStaleViews)
{
  auto &auth = auth_server();
  auth.register_auth_data_updates(DBAuthTip{.block_id = 100, .height = 0},
                                  {})();
  ASSERT_EQ(auth.open_block(100, 101), RPCError::kRPCNone);
  ASSERT_EQ(auth.open_block(100, 102), RPCError::kRPCNone);
  ASSERT_EQ(auth.close_block(101)(), RPCError::kRPCNone);
  auth.register_auth_data_updates(DBAuthTip{.block_id = 101, .height = 1},
                                  {})();
  EXPECT_EQ(auth.commit_block(101)(), RPCError::kRPCNone);
  EXPECT_EQ(auth.verify_batch(102, {})(), AuthErrorCode::kAuthStaleBlockHeight);
  EXPECT_EQ(auth.abandon_block(102)(), RPCError::kRPCNone);
}

class BlockingLookupDBAuthRpc : public MockDBAuthRpc {
public:
  x23::Async<DBAuthResult> authdb_get_auth_data(BlockHeight, ContractAddress)
  {
    entered.store(true);
    entered.notify_all();
    while (!released.load()) {
      co_await released.wait(false);
    }
    co_return DBAuthResult(DBAuthError::kDBAuthNotFound);
  }
  std::atomic<bool> entered{false};
  x23::CoroFutex<bool> released{false};
};

TEST_F(AuthBlockTest, CloseAndAbandonDrainAdmittedVerification)
{
  RpcServer<BlockingLookupDBAuthRpc> db;
  const auto port = db.start_server(0);
  auto client = make_dbauth_client(std::format("localhost:{}", port))().value();
  AuthServer auth(std::move(client), NetworkHash{}, 16);
  auth.register_auth_data_updates(DBAuthTip{.block_id = 100, .height = 0},
                                  {})();
  ASSERT_EQ(auth.open_block(100, 101), RPCError::kRPCNone);
  SignedTransaction tx{};
  auto verification = auth.verify_transactions(101, std::span(&tx, 1));
  db->impl()->entered.wait(false);
  auto close = auth.close_block(101);
  EXPECT_FALSE(close.operator co_await().await_ready());
  EXPECT_EQ(auth.verify_batch(101, {})(), AuthErrorCode::kAuthBadBlockID);
  auto abandon = auth.abandon_block(101);
  EXPECT_FALSE(abandon.operator co_await().await_ready());
  db->impl()->released.store(true);
  db->impl()->released.broadcast(&default_threadpool());
  EXPECT_EQ(verification().size(), 1);
  EXPECT_EQ(close(), RPCError::kRPCBadBlockID);
  EXPECT_EQ(abandon(), RPCError::kRPCNone);
}

class BlockingMempoolAuthRpc {
public:
  using rpc_service = MempoolAuth1;
  static x23::Async<IntroAuthResult> mempoolauth_intro_auth(AuthCard)
  {
    co_return IntroAuthResult(IntroAuthOutcome::kSuccess);
  }
  x23::Async<void> mempoolauth_auth_data_changed(BlockHeight, KeyChangeBatch)
  {
    entered.store(true);
    entered.notify_all();
    while (!released.load()) {
      co_await released.wait(false);
    }
  }
  std::atomic<bool> entered{false};
  x23::CoroFutex<bool> released{false};
};

TEST_F(AuthBlockTest, CommitDoesNotWaitForMempoolAcknowledgement)
{
  auto &auth = auth_server();
  auth.register_auth_data_updates(DBAuthTip{.block_id = 100, .height = 0},
                                  {})();
  RpcServer<BlockingMempoolAuthRpc> mempool;
  const auto port = mempool.start_server(0);
  x23::HostPort address;
  ASSERT_TRUE(address.parse(std::format("localhost:{}", port)));
  auto client = make_rpc_client<MempoolAuth1>(address)();
  ASSERT_TRUE(client);
  auth.set_mempool_client(std::move(*client));
  ASSERT_EQ(auth.open_block(100, 101), RPCError::kRPCNone);
  ASSERT_EQ(auth.close_block(101)(), RPCError::kRPCNone);
  auto commit = auth.commit_block(101);
  auto update =
      auth.register_auth_data_updates(DBAuthTip{.block_id = 101, .height = 1},
                                      {});
  mempool->impl()->entered.wait(false);
  EXPECT_EQ(commit(), RPCError::kRPCNone);
  EXPECT_FALSE(update.operator co_await().await_ready());
  mempool->impl()->released.store(true);
  mempool->impl()->released.broadcast(&default_threadpool());
  update();
}

} // namespace
} // namespace hogd
