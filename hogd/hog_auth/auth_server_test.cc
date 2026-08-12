#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

#include "address_auth.hh"
#include "auth_rpc.hh"
#include "hogd/hog_auth/auth_server_test_framework.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"

#include <cstddef>
#include <format>
#include <string>
#include <utility>

#include "auth_card.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hog_auth/dbauth_client.h"
#include "rpc_types.hh"
#include "x23/x23.h"

namespace hogd {

class AuthServerTest : public ::testing::Test, public AuthServerTestFramework {
protected:
private:
};
// NOLINTBEGIN(bugprone-unchecked-optional-access)

// Test basic lookup of auth data that exists in the mock DB
TEST_F(AuthServerTest, BasicLookup)
{
  ContractAddress address = make_test_address(1);
  AddressAuthData auth_data = make_test_auth_data(1);

  mock_db().increment_height(); // height 1
  mock_db().set(address, auth_data);
  mock_db().increment_height(); // now at height 2

  BlockHeight height = mock_db().get_current_height();
  ASSERT_EQ(height, 2);

  auth_server().register_auth_data_updates(height,
                                           mock_db().get_key_changes_at_height(
                                               1))();

  auto result = auth_server().lookup_auth_data(height, address)();
  ASSERT_TRUE(result.has_value());
  ASSERT_NE(result.value(), nullptr);
  EXPECT_EQ(result.value()->first, height);
  ASSERT_TRUE(result.value()->second.has_value());
}

// Test lookup of non-existent auth data
TEST_F(AuthServerTest, LookupNonExistent)
{
  ContractAddress address = make_test_address(2);

  mock_db().increment_height(); // height 1
  mock_db().increment_height(); // height 2
  // initialize the auth server at height 2
  auth_server().register_auth_data_updates(2,
                                           mock_db().get_key_changes_at_height(
                                               1))();

  auto result =
      auth_server().lookup_auth_data(mock_db().get_current_height(), address)();

  ASSERT_TRUE(result.has_value());
  ASSERT_NE(result.value(), nullptr);
  EXPECT_EQ(result.value()->first, mock_db().get_current_height());
  EXPECT_FALSE(result.value()->second.has_value());
}

TEST_F(AuthServerTest, OutOfOrderUpdates)
{
  ContractAddress address = make_test_address(3);
  AddressAuthData auth_data = make_test_auth_data(1);
  AddressAuthData auth_data_2 = make_test_auth_data(2);

  mock_db().increment_height(); // height 1
  mock_db().set(address, auth_data);

  mock_db().increment_height(); // height 2
  mock_db().set(address, auth_data_2);

  mock_db().increment_height(); // height 3

  // make sure the auth server starts initialized at height 1
  auth_server().register_auth_data_updates(1,
                                           mock_db().get_key_changes_at_height(
                                               1))();

  std::jthread thread([&] {
    auth_server().register_auth_data_updates(
        3, mock_db().get_key_changes_at_height(3))();
  });
  // let that thread start running for a bit
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auth_server().register_auth_data_updates(2,
                                           mock_db().get_key_changes_at_height(
                                               2))();

  thread.join();

  auto result =
      auth_server().lookup_auth_data(mock_db().get_current_height(), address)();
  ASSERT_EQ(mock_db().get_current_height(), 3);

  ASSERT_TRUE(result.has_value());

  ASSERT_NE(result.value(), nullptr);
  EXPECT_EQ(result.value()->first, 2);
  ASSERT_TRUE(result.value()->second.has_value());
  EXPECT_EQ(result.value()->second.value(), auth_data_2);
}

TEST_F(AuthServerTest, StaleBlockHeight)
{
  ContractAddress address = make_test_address(4);
  AddressAuthData auth_data = make_test_auth_data(1);

  mock_db().increment_height(); // height 1

  auto key_changes = mock_db().get_key_changes_at_height(1);
  ASSERT_EQ(key_changes.size(), 0);
  auth_server().register_auth_data_updates(1, key_changes)();

  auto result = auth_server().lookup_auth_data(1, address)();

  ASSERT_TRUE(result.has_value());
  ASSERT_NE(result.value(), nullptr);
  EXPECT_EQ(result.value()->first, 1);
  ASSERT_FALSE(result.value()->second.has_value());

  mock_db().increment_height(); // height 2
  mock_db().set(address, auth_data);
  mock_db().compute_key_changes_for_current_height();

  ASSERT_EQ(mock_db().get_key_changes_at_height(2).size(), 1);

  // it's ok to get cached data if the auth server hasn't gotten the next batch
  // of key changes yet
  result = auth_server().lookup_auth_data(1, address)();
  ASSERT_TRUE(result.has_value());
  ASSERT_NE(result.value(), nullptr);
  EXPECT_EQ(result.value()->first, 1);
  ASSERT_FALSE(result.value()->second.has_value());

  result = auth_server().lookup_auth_data(1, make_test_address(5))();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), AuthErrorCode::kAuthStaleBlockHeight);

  auth_server().register_auth_data_updates(2,
                                           mock_db().get_key_changes_at_height(
                                               2))();

  // now get the actual data
  result = auth_server().lookup_auth_data(2, address)();
  ASSERT_TRUE(result.has_value());
  ASSERT_NE(result.value(), nullptr);
  EXPECT_EQ(result.value()->first, 2);
  ASSERT_TRUE(result.value()->second.has_value());
  EXPECT_EQ(result.value()->second.value(), auth_data);

  result = auth_server().lookup_auth_data(2, make_test_address(5))();
  ASSERT_TRUE(result.has_value());
  ASSERT_NE(result.value(), nullptr);
  EXPECT_EQ(result.value()->first, 2);
  ASSERT_FALSE(result.value()->second.has_value());
}

TEST_F(AuthServerTest, QueriesDelayed)
{
  ContractAddress address = make_test_address(6);
  AddressAuthData auth_data = make_test_auth_data(1);

  mock_db().increment_height(); // 1
  mock_db().increment_height(); // 2
  mock_db().increment_height(); // 3
  mock_db().increment_height(); // 4
  mock_db().increment_height(); // 5
  mock_db().compute_key_changes_for_current_height();

  std::atomic<bool> sequence = false;

  std::jthread thread([&] {
    sequence = true;
    sequence.notify_all();
    auto res = auth_server().lookup_auth_data(5, address)();
  });

  // hack to make sure the query thread is actually waiting on the coro futex
  sequence.wait(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auth_server().register_auth_data_updates(1,
                                           mock_db().get_key_changes_at_height(
                                               1))();
  auth_server().register_auth_data_updates(2,
                                           mock_db().get_key_changes_at_height(
                                               2))();
  auth_server().register_auth_data_updates(3,
                                           mock_db().get_key_changes_at_height(
                                               3))();
  auth_server().register_auth_data_updates(4,
                                           mock_db().get_key_changes_at_height(
                                               4))();
  sequence = true;
  auth_server().register_auth_data_updates(5,
                                           mock_db().get_key_changes_at_height(
                                               5))();

  thread.join();
}

TEST_F(AuthServerTest, QueriesDelayedFromInitialized)
{
  ContractAddress address = make_test_address(6);
  AddressAuthData auth_data = make_test_auth_data(1);

  mock_db().increment_height(); // 1
  mock_db().increment_height(); // 2
  mock_db().increment_height(); // 3
  mock_db().increment_height(); // 4
  mock_db().increment_height(); // 5
  mock_db().compute_key_changes_for_current_height();

  // Unlike QueriesDelayed, start with an auth server that's properly
  // initialized, but just missing recent updates.

  auth_server().register_auth_data_updates(1,
                                           mock_db().get_key_changes_at_height(
                                               1))();
  auth_server().register_auth_data_updates(2,
                                           mock_db().get_key_changes_at_height(
                                               2))();

  std::atomic<bool> sequence = false;

  std::jthread thread([&] {
    sequence = true;
    sequence.notify_all();
    auto res = auth_server().lookup_auth_data(5, address)();
  });

  // hack to make sure the query thread is actually waiting on the coro futex
  sequence.wait(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auth_server().register_auth_data_updates(3,
                                           mock_db().get_key_changes_at_height(
                                               3))();
  // make sure that the waiting thread wakes up and then goes back to sleep
  // TODO(geoff): I don't have a good way to test whether the thread actually
  // wakes and goes back to sleep.  That's not required for correct external
  // behavior of the auth server, but I do want to test the logic for when
  // threads decide to stop waiting.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  auth_server().register_auth_data_updates(4,
                                           mock_db().get_key_changes_at_height(
                                               4))();
  sequence = true;
  auth_server().register_auth_data_updates(5,
                                           mock_db().get_key_changes_at_height(
                                               5))();

  thread.join();
}

TEST_F(AuthServerTest, InitializeFromLateBlock)
{
  ContractAddress address = make_test_address(7);
  AddressAuthData auth_data = make_test_auth_data(1);

  mock_db().set(address, auth_data);

  mock_db().increment_height(); // height 1
  mock_db().increment_height(); // height 2
  mock_db().increment_height(); // height 3

  mock_db().delete_address(address);
  mock_db().compute_key_changes_for_current_height();

  auth_server().register_auth_data_updates(3,
                                           mock_db().get_key_changes_at_height(
                                               3))();

  auto result = auth_server().lookup_auth_data(3, address)();
  ASSERT_TRUE(result.has_value());
  ASSERT_NE(result.value(), nullptr);
  EXPECT_EQ(result.value()->first, 3);
  ASSERT_FALSE(result.value()->second.has_value());

  // stale block updates don't break things
  auth_server().register_auth_data_updates(1,
                                           mock_db().get_key_changes_at_height(
                                               1))();

  result = auth_server().lookup_auth_data(3, address)();
  ASSERT_TRUE(result.has_value());
  ASSERT_NE(result.value(), nullptr);
  EXPECT_EQ(result.value()->first, 3);
  ASSERT_FALSE(result.value()->second.has_value());
}

namespace {

class BlockingIntroDBAuthRpc {
public:
  using rpc_service = DBAuth1;

  static x23::Async<DBAuthResult>
  authdb_get_auth_data(BlockHeight /*height*/, ContractAddress /*address*/)
  {
    co_return DBAuthResult(DBAuthError::kDBAuthNotFound);
  }

  static x23::Async<DBAuthResultBatch>
  authdb_get_auth_data_batch(BlockHeight /*height*/,
                             ContractAddressBatch const &addresses)
  {
    DBAuthResultBatch result;
    result.reserve(addresses.size());
    for (size_t i = 0; i < addresses.size(); ++i) {
      result.emplace_back(DBAuthError::kDBAuthNotFound);
    }
    co_return result;
  }

  x23::Async<DBAuthIntroResult> dbauth_intro(AuthCard /*card*/)
  {
    intro_started_.store(true);
    intro_started_.notify_all();
    release_intro_.wait(false);

    DBAuthIntroResult ret(DBAuthIntroOutcome::kIntroSuccess);
    ret.current_height() = return_height_;
    co_return ret;
  }

  void wait_for_intro() { intro_started_.wait(false); }

  void release_intro(BlockHeight return_height)
  {
    return_height_ = return_height;
    release_intro_.store(true);
    release_intro_.notify_all();
  }

private:
  std::atomic<bool> intro_started_ = false;
  std::atomic<bool> release_intro_ = false;
  BlockHeight return_height_ = -1;
};

static_assert(is_DBAuth1<BlockingIntroDBAuthRpc *, x23::Async>);

} // namespace

TEST_F(AuthServerTest, DbAuthIntroUpdateRace)
{
  RpcServer<BlockingIntroDBAuthRpc> db_server;
  const uint16_t db_port = db_server.start_server(0);
  const std::string db_address = std::format("localhost:{}", db_port);
  dbauth_client_t dbauth_client = make_dbauth_client(db_address)().value();
  AuthServer race_auth_server(std::move(dbauth_client), 16);

  AuthCard auth_card{.protocol = {},
                     .upid = {},
                     .address_for_db = "",
                     .address_for_mempool = ""};

  std::jthread subscribe_thread(
      [&] { race_auth_server.subscribe_to_auth_data_changes(auth_card)(); });

  db_server->impl()->wait_for_intro();

  x23::Ptr<AddressAuthData> auth_data_ptr;
  auth_data_ptr.emplace(make_test_auth_data(1));

  KeyChangeBatch key_changes;
  key_changes.push_back(KeyChange{.address = make_test_address(99),
                                  .auth_data = std::move(auth_data_ptr)});
  race_auth_server.register_auth_data_updates(1, std::move(key_changes))();

  db_server->impl()->release_intro(1);
  subscribe_thread.join();

  // The racing update should have won initialization — verify by checking
  // that a lookup at height 1 succeeds (futex is at height 1).
  auto result = race_auth_server.lookup_auth_data(1, make_test_address(99))();
  ASSERT_TRUE(result.has_value());
  ASSERT_NE(result.value(), nullptr);
  EXPECT_EQ(result.value()->first, 1);
  ASSERT_TRUE(result.value()->second.has_value());
}

namespace {

class SimpleDBAuthRpc {
public:
  using rpc_service = DBAuth1;

  explicit SimpleDBAuthRpc(BlockHeight intro_height)
    : intro_height_(intro_height)
  {}

  x23::Async<DBAuthResult>
  authdb_get_auth_data(BlockHeight height, ContractAddress /*address*/) const
  {
    if (height != intro_height_) {
      co_return DBAuthResult(DBAuthError::kDBAuthStaleBlockHeight);
    }
    co_return DBAuthResult(DBAuthError::kDBAuthNotFound);
  }

  x23::Async<DBAuthResultBatch>
  authdb_get_auth_data_batch(BlockHeight height,
                             ContractAddressBatch const &addresses) const
  {
    DBAuthResultBatch result;
    result.reserve(addresses.size());
    for (size_t i = 0; i < addresses.size(); i++) {
      result.emplace_back(height == intro_height_
                              ? DBAuthError::kDBAuthNotFound
                              : DBAuthError::kDBAuthStaleBlockHeight);
    }
    co_return result;
  }

  x23::Async<DBAuthIntroResult> dbauth_intro(AuthCard /*card*/) const
  {
    DBAuthIntroResult ret(DBAuthIntroOutcome::kIntroSuccess);
    ret.current_height() = intro_height_;
    co_return ret;
  }

private:
  BlockHeight intro_height_;
};

static_assert(is_DBAuth1<SimpleDBAuthRpc *, x23::Async>);

} // namespace

// Regression test for #1448: lookup_auth_data waiters stranded when
// subscribe_to_auth_data_changes stores the initial height without broadcast.
TEST_F(AuthServerTest, SubscribeBroadcastWakesLookupWaiters)
{
  constexpr BlockHeight kIntroHeight = 5;
  ContractAddress address = make_test_address(10);

  RpcServer<SimpleDBAuthRpc> db_server(kIntroHeight);
  uint16_t port = db_server.start_server(0);
  std::string db_address = std::format("localhost:{}", port);
  dbauth_client_t dbauth_client = make_dbauth_client(db_address)().value();
  AuthServer auth_server(std::move(dbauth_client), 16);

  std::atomic<bool> lookup_started = false;
  std::atomic<bool> lookup_finished = false;

  std::jthread lookup_thread([&] {
    lookup_started = true;
    lookup_started.notify_all();
    auto res = auth_server.lookup_auth_data(kIntroHeight, address)();
    lookup_finished = true;
    lookup_finished.notify_all();
  });

  lookup_started.wait(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(lookup_finished.load());

  AuthCard card{};
  auth_server.subscribe_to_auth_data_changes(card)();

  lookup_finished.wait(false);
  EXPECT_TRUE(lookup_finished.load());

  lookup_thread.join();
}

TEST_F(AuthServerTest, CacheEntryLifetimeSurvivesReplacement)
{
  const ContractAddress address = make_test_address(101);
  AddressAuthData old_auth_data = make_test_auth_data(1);
  old_auth_data.last_written_height = 11;
  AddressAuthData new_auth_data = make_test_auth_data(2);
  new_auth_data.last_written_height = 22;

  mock_db().set(address, old_auth_data);
  mock_db().compute_key_changes_for_current_height();
  auth_server().register_auth_data_updates(0,
                                           mock_db().get_key_changes_at_height(
                                               0))();

  auto lookup = auth_server().lookup_auth_data(0, address)();
  ASSERT_TRUE(lookup.has_value());
  ASSERT_NE(lookup.value(), nullptr);
  ASSERT_TRUE(lookup.value()->second.has_value());

  auto retained_entry = std::move(lookup.value());
  const AddressAuthData *raw_ptr = &retained_entry->second.value();

  x23::Ptr<AddressAuthData> updated_auth_data;
  updated_auth_data.emplace(new_auth_data);
  KeyChangeBatch key_changes;
  key_changes.push_back(
      KeyChange{.address = address, .auth_data = std::move(updated_auth_data)});
  auth_server().register_auth_data_updates(1, std::move(key_changes))();

  EXPECT_EQ(raw_ptr->last_written_height, 11);
  EXPECT_EQ(retained_entry->second.value(), old_auth_data);
}

TEST_F(AuthServerTest, VerifyTransactionsBasicSmoke)
{
  const ContractAddress address = make_test_address(50);
  mock_db().set(address, make_test_auth_data(1));
  mock_db().compute_key_changes_for_current_height();
  auth_server().register_auth_data_updates(0,
                                           mock_db().get_key_changes_at_height(
                                               0))();

  SignedTransaction stx{};
  stx.tx.invocation.invoked_address = address;

  auto results = auth_server().verify_transactions(0, std::span(&stx, 1))();
  ASSERT_EQ(results.size(), 1);
}

TEST_F(AuthServerTest, VerifyTransactionsSurvivesConcurrentCacheReplacement)
{
  const ContractAddress address = make_test_address(50);
  mock_db().set(address, make_test_auth_data(1));
  mock_db().compute_key_changes_for_current_height();
  auth_server().register_auth_data_updates(0,
                                           mock_db().get_key_changes_at_height(
                                               0))();

  SignedTransaction stx{};
  stx.tx.invocation.invoked_address = address;

  std::atomic<BlockHeight> current_h = 0;
  std::atomic<bool> done = false;

  std::jthread replacer([&] {
    for (BlockHeight h = 1; !done.load(std::memory_order_relaxed); h++) {
      x23::Ptr<AddressAuthData> ptr;
      ptr.emplace(make_test_auth_data(static_cast<uint8_t>((h % 20) + 1)));
      KeyChangeBatch changes;
      changes.push_back(
          KeyChange{.address = address, .auth_data = std::move(ptr)});
      auth_server().register_auth_data_updates(h, std::move(changes))();
      current_h.store(h, std::memory_order_release);
    }
  });

  for (int i = 0; i < 1000; i++) {
    BlockHeight h = current_h.load(std::memory_order_acquire);
    auth_server().verify_transactions(h, std::span(&stx, 1))();
  }
  done = true;
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
