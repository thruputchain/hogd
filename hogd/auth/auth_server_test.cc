#include <gtest/gtest.h>

#include <optional>

#include "account.hh"
#include "hogd/auth/auth_server_test_framework.h"
#include "hogd/base/types.h"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

namespace {

constexpr ContractAddress
make_test_address(uint8_t id)
{
  ContractAddress address{};
  address[0] = id;
  return address;
}

AccountAuthData
make_test_auth_data()
{
  AccountAuthData auth_data{};
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
  AccountAuthData auth_data = make_test_auth_data();

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

} // namespace hogd
