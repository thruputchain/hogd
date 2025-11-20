#include <gtest/gtest.h>

#include <optional>

#include "account.hh"
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
  PublicKey key;
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
  AuthServerTest() = default;

private:
};

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
                                               height));

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

  mock_db().set(address, auth_data);
  mock_db().increment_height();

  mock_db().set(address, auth_data_2);
  mock_db().increment_height();

  std::jthread thread([&] {
    auth_server().register_auth_data_updates(
        2, mock_db().get_key_changes_at_height(2));
  });
  // let that thread start running for a bit
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auth_server().register_auth_data_updates(1,
                                           mock_db().get_key_changes_at_height(
                                               1));

  thread.join();

  auto result =
      auth_server().lookup_auth_data(mock_db().get_current_height(), address)();

  ASSERT_TRUE(result.has_value());
  ASSERT_NE(result.value(), nullptr);
  EXPECT_EQ(result.value()->first, mock_db().get_current_height());
  ASSERT_TRUE(result.value()->second.has_value());
  EXPECT_EQ(result.value()->second.value(), auth_data_2);
}

} // namespace hogd
