#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>

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

TEST(AuthTokenRegistryTest, UnwrapTwiceReturnsSameResult)
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
                                      .child_nodes = {}}}}};
  auto token_id_opt = registry.register_auth_token(auth_token);
  ASSERT_TRUE(token_id_opt.has_value());

  auto first_unwrap = registry.unwrap_auth_token(token_id_opt.value());
  ASSERT_TRUE(first_unwrap.has_value());

  auto second_unwrap = registry.unwrap_auth_token(token_id_opt.value());
  ASSERT_TRUE(second_unwrap.has_value());

  ASSERT_EQ(first_unwrap->new_token_id_start,
            second_unwrap->new_token_id_start);
  ASSERT_EQ(first_unwrap->new_token_id_count,
            second_unwrap->new_token_id_count);
}

// Pins the exact id-allocation contract that issue #2543 flagged (and that is
// working as intended): register_auth_token and unwrap_auth_token draw ids from
// a single, monotonically increasing counter, and unwrap is idempotent. A
// second unwrap of the same token reuses the cached child range and allocates
// no new ids, so a register interleaved between two unwraps never collides with
// the unwrap's children and the counter never moves backwards.
TEST(AuthTokenRegistryTest, UnwrapAndRegisterShareOneMonotonicIdSpace)
{
  AuthTokenRegistry registry;

  auto leaf = [](uint32_t index) {
    return AuthTokenNode{.consumer_address = make_test_address(index),
                         .payload = make_test_payload(index),
                         .child_nodes = {}};
  };
  auto token_with_children =
      [&](std::initializer_list<AuthTokenNode> children) {
        return AuthToken{.mint_address = make_test_address(1),
                         .expiry_height = 100,
                         .root_node =
                             AuthTokenNode{.consumer_address =
                                               make_test_address(2),
                                           .payload = make_test_payload(0ULL),
                                           .child_nodes = children}};
      };

  // Root token gets id 0.
  auto root_id =
      registry.register_auth_token(token_with_children({leaf(3), leaf(4)}));
  ASSERT_TRUE(root_id.has_value());
  ASSERT_EQ(root_id.value(), 0U);

  // First unwrap allocates ids 1 and 2 for the two children and advances the
  // shared counter past them.
  auto first = registry.unwrap_auth_token(root_id.value());
  ASSERT_TRUE(first.has_value());
  ASSERT_EQ(first->new_token_id_start, 1U);
  ASSERT_EQ(first->new_token_id_count, 2U);

  // A register issued after the unwrap continues from the advanced counter and
  // gets id 3 -- it does not reuse or collide with child ids 1 and 2.
  auto mid_id = registry.register_auth_token(token_with_children({}));
  ASSERT_TRUE(mid_id.has_value());
  ASSERT_EQ(mid_id.value(), 3U);

  // Re-unwrapping the root is idempotent: it returns the cached child range
  // (start 1, count 2) and allocates no further ids.
  auto second = registry.unwrap_auth_token(root_id.value());
  ASSERT_TRUE(second.has_value());
  ASSERT_EQ(second->new_token_id_start, first->new_token_id_start);
  ASSERT_EQ(second->new_token_id_count, first->new_token_id_count);

  // The idempotent re-unwrap left the counter untouched, so the next register
  // is id 4 -- immediately after mid_id, with no gap for re-allocated children.
  auto next_id = registry.register_auth_token(token_with_children({}));
  ASSERT_TRUE(next_id.has_value());
  ASSERT_EQ(next_id.value(), 4U);
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
