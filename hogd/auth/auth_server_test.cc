#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

#include "account.hh"
#include "auth_rpc.hh"
#include "hogd/auth/auth_server_test_framework.h"
#include "hogd/base/types.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/x23.h"

namespace hogd {

namespace {

constexpr ContractAddress
make_test_address(uint8_t id)
{
  ContractAddress address{};
  address[0] = id;
  return address;
}

PublicKeyConfigOpt
make_test_pk_config(uint8_t id)
{
  PublicKey key(CryptographicScheme::Ed25519);
  key.ed25519().fill(id);
  return x23::Ptr<PublicKeyConfig>(
      new PublicKeyConfig{.key = key, .max_weight = 100});
}

AccountAuthData
make_test_auth_data(uint8_t id)
{
  AccountAuthData auth_data{};
  auth_data.public_keys[id] = make_test_pk_config(id);
  auth_data.last_written_height = 0;
  return auth_data;
}

} // namespace

class AuthServerTest : public ::testing::Test, public AuthServerTestFramework {
protected:
private:
};
// NOLINTBEGIN(bugprone-unchecked-optional-access)

// Test basic lookup of auth data that exists in the mock DB
TEST_F(AuthServerTest, BasicLookup)
{
  ContractAddress address = make_test_address(1);
  AccountAuthData auth_data = make_test_auth_data(1);

  mock_db().set(address, auth_data);
  mock_db().increment_height();

  BlockHeight height = mock_db().get_current_height();
  ASSERT_EQ(height, 1);

  auth_server().register_auth_data_updates(height,
                                           mock_db().get_key_changes_at_height(
                                               height))();

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
  // initialize the auth server at height 1
  auth_server().register_auth_data_updates(1,
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
  AccountAuthData auth_data = make_test_auth_data(1);
  AccountAuthData auth_data_2 = make_test_auth_data(2);

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
  EXPECT_EQ(result.value()->first, mock_db().get_current_height());
  ASSERT_TRUE(result.value()->second.has_value());
  EXPECT_EQ(result.value()->second.value(), auth_data_2);
}

TEST_F(AuthServerTest, StaleBlockHeight)
{
  ContractAddress address = make_test_address(4);
  AccountAuthData auth_data = make_test_auth_data(1);

  mock_db().increment_height();

  auto key_changes = mock_db().get_key_changes_at_height(1);
  auth_server().register_auth_data_updates(1, key_changes)();

  auto result = auth_server().lookup_auth_data(1, address)();

  ASSERT_TRUE(result.has_value());
  ASSERT_NE(result.value(), nullptr);
  EXPECT_EQ(result.value()->first, 1);
  ASSERT_FALSE(result.value()->second.has_value());

  mock_db().set(address, auth_data);
  mock_db().increment_height(); // height 2

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
  AccountAuthData auth_data = make_test_auth_data(1);

  mock_db().increment_height(); // 1
  mock_db().increment_height(); // 2
  mock_db().increment_height(); // 3
  mock_db().increment_height(); // 4
  mock_db().increment_height(); // 5

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
  AccountAuthData auth_data = make_test_auth_data(1);

  mock_db().increment_height(); // 1
  mock_db().increment_height(); // 2
  mock_db().increment_height(); // 3
  mock_db().increment_height(); // 4
  mock_db().increment_height(); // 5

  // Unlike QueriesDelayed, start with an auth server that's properly initialized,
  // but just missing recent updates.

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
  // TODO(geoff): I don't have a good way to test whether the thread actually wakes and 
  // goes back to sleep.  That's not required for correct external behavior of the auth server,
  // but I do want to test the logic for when threads decide to stop waiting.
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
  AccountAuthData auth_data = make_test_auth_data(1);

  mock_db().set(address, auth_data);

  mock_db().increment_height(); // height 1
  mock_db().increment_height(); // height 2
  mock_db().increment_height(); // height 3

  mock_db().delete_address(address);
  mock_db().increment_height(); // height 4

  auth_server().register_auth_data_updates(4,
                                           mock_db().get_key_changes_at_height(
                                               4))();

  auto result = auth_server().lookup_auth_data(4, address)();
  ASSERT_TRUE(result.has_value());
  ASSERT_NE(result.value(), nullptr);
  EXPECT_EQ(result.value()->first, 4);
  ASSERT_FALSE(result.value()->second.has_value());

  // stale block updates don't break things
  auth_server().register_auth_data_updates(1,
                                           mock_db().get_key_changes_at_height(
                                               1))();

  result = auth_server().lookup_auth_data(4, address)();
  ASSERT_TRUE(result.has_value());
  ASSERT_NE(result.value(), nullptr);
  EXPECT_EQ(result.value()->first, 4);
  ASSERT_FALSE(result.value()->second.has_value());
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
