#include <gtest/gtest.h>

#include <algorithm>

#include "auth_token.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/components/auth_token_registry.h"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {

namespace {
ContractAddress
make_test_address(uint32_t index)
{
  auto hash = sha256(index);
  ContractAddress address;
  std::ranges::copy(hash, address.begin());
  return address;
}

AuthTokenPayload
make_test_payload(uint32_t index)
{
  return x23::serialize(index);
}
} // namespace

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST(AuthTokenRegistryTest, BasicTest)
{
  AuthTokenRegistry registry;
  AuthToken auth_token =
      AuthToken{.mint_address = make_test_address(1),
                .expiry_height = 100,
                .root_node =
                    AuthTokenNode{.consumer_address = make_test_address(2),
                                  .payload = make_test_payload(0ULL),
                                  .child_nodes = {}}};
  auto token_id_opt = registry.register_auth_token(auth_token);
  ASSERT_TRUE(token_id_opt.has_value());
  uint32_t token_id = token_id_opt.value();
  ASSERT_EQ(token_id, 0);
  AuthToken const *result = registry.lookup_auth_token(token_id);
  ASSERT_NE(result, nullptr);
  ASSERT_EQ(result->mint_address, make_test_address(1));
  ASSERT_EQ(result->expiry_height, 100);
  ASSERT_EQ(result->root_node.consumer_address, make_test_address(2));
  ASSERT_EQ(result->root_node.payload, make_test_payload(0ULL));
  ASSERT_EQ(result->root_node.child_nodes.size(), 0);
}

TEST(AuthTokenRegistryTest, UnwrapTest)
{
  AuthTokenRegistry registry;
  AuthToken auth_token =
      AuthToken{.mint_address = make_test_address(1),
                .expiry_height = 100,
                .root_node = AuthTokenNode{
                    .consumer_address = make_test_address(2),
                    .payload = make_test_payload(0ULL),
                    .child_nodes = {
                        AuthTokenNode{.consumer_address = make_test_address(3),
                                      .payload = make_test_payload(1ULL),
                                      .child_nodes = {}},
                        AuthTokenNode{.consumer_address = make_test_address(4),
                                      .payload = make_test_payload(2ULL),
                                      .child_nodes = {}},
                        AuthTokenNode{.consumer_address = make_test_address(5),
                                      .payload = make_test_payload(3ULL),
                                      .child_nodes = {}}}}};
  auto token_id_opt = registry.register_auth_token(auth_token);
  ASSERT_TRUE(token_id_opt.has_value());
  uint32_t token_id = token_id_opt.value();
  ASSERT_EQ(token_id, 0);
  auto res = registry.unwrap_auth_token(token_id);
  ASSERT_TRUE(res.has_value());
  AuthTokenUnwrapResult result = res.value();
  ASSERT_EQ(result.new_token_id_start, 1);
  ASSERT_EQ(result.new_token_id_count, 3);
  AuthToken const *result_1 =
      registry.lookup_auth_token(result.new_token_id_start);
  ASSERT_NE(result_1, nullptr);
  ASSERT_EQ(result_1->mint_address, make_test_address(1));
  ASSERT_EQ(result_1->expiry_height, 100);
  ASSERT_EQ(result_1->root_node.consumer_address, make_test_address(3));
  ASSERT_EQ(result_1->root_node.payload, make_test_payload(1ULL));
  ASSERT_EQ(result_1->root_node.child_nodes.size(), 0);
  AuthToken const *result_2 =
      registry.lookup_auth_token(result.new_token_id_start + 1);
  ASSERT_NE(result_2, nullptr);
  ASSERT_EQ(result_2->mint_address, make_test_address(1));
  ASSERT_EQ(result_2->expiry_height, 100);
  ASSERT_EQ(result_2->root_node.consumer_address, make_test_address(4));
  ASSERT_EQ(result_2->root_node.payload, make_test_payload(2ULL));
  ASSERT_EQ(result_2->root_node.child_nodes.size(), 0);
  AuthToken const *result_3 =
      registry.lookup_auth_token(result.new_token_id_start + 2);
  ASSERT_NE(result_3, nullptr);
  ASSERT_EQ(result_3->mint_address, make_test_address(1));
  ASSERT_EQ(result_3->expiry_height, 100);
  ASSERT_EQ(result_3->root_node.consumer_address, make_test_address(5));
  ASSERT_EQ(result_3->root_node.payload, make_test_payload(3ULL));
  ASSERT_EQ(result_3->root_node.child_nodes.size(), 0);
  AuthToken const *result_4 =
      registry.lookup_auth_token(result.new_token_id_start + 3);
  ASSERT_EQ(result_4, nullptr);
}
// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
