#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "auth_token.hh"
#include "hogd/base/rc.h"
#include "hogd/hog_exec/components/auth_subtoken.h"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

// Minting decodes under the chain's depth limit, which defaults to this.
constexpr size_t kDepth = kAuthTokenMaxDepth;

using Bytes = std::vector<uint8_t>;

ContractAddress
address(uint8_t id)
{
  ContractAddress a{};
  a.fill(id);
  return a;
}

AuthTokenNode
node(uint8_t id, size_t payload_len, std::vector<AuthTokenNode> children = {})
{
  AuthTokenNode n{.consumer_address = address(id)};
  n.payload.resize(payload_len, id);
  for (auto &child : children) {
    n.child_nodes.push_back(std::move(child));
  }
  return n;
}

// levels nodes, each the only child of the one above.
AuthTokenNode
chain(size_t levels)
{
  AuthTokenNode n = node(0, 1);
  for (size_t i = 1; i < levels; ++i) {
    n = node(static_cast<uint8_t>(i), 1, {std::move(n)});
  }
  return n;
}

AuthToken
token(AuthTokenNode root)
{
  return AuthToken{.mint_address = address(0xee),
                   .expiry_height = 1234,
                   .root_node = std::move(root)};
}

Bytes
encode(auto const &x)
{
  auto buf = x23::serialize(x);
  return {buf.begin(), buf.end()};
}

// What x23 makes of bytes decoded as an X: the value, re-encoded, or nullopt
// if it rejects them.
template<typename X>
std::optional<Bytes>
decode_as(Bytes const &bytes, size_t depth)
{
  X x;
  try {
    x23::deserialize(&x, x23::BufView{bytes.data(), bytes.size()}, depth);
  } catch (x23::XdrException const &) {
    return std::nullopt;
  }
  return encode(x);
}

// Decoding into the shared types must accept exactly what the generated types
// accept, and decode to the same value.
void
expect_same_node_decode(Bytes const &bytes, size_t depth = kDepth)
{
  EXPECT_EQ(decode_as<Rc<AuthSubtoken>>(bytes, depth),
            decode_as<AuthTokenNode>(bytes, depth))
      << "input of " << bytes.size() << " bytes";
}

void
expect_same_token_decode(Bytes const &bytes, size_t depth = kDepth)
{
  EXPECT_EQ(decode_as<RegisteredAuthToken>(bytes, depth),
            decode_as<AuthToken>(bytes, depth))
      << "input of " << bytes.size() << " bytes";
}

Rc<AuthSubtoken>
decode_node(AuthTokenNode const &n)
{
  Rc<AuthSubtoken> root;
  x23::deserialize(&root, x23::serialize(n));
  return root;
}

void
put32(Bytes &bytes, size_t offset, uint32_t value)
{
  for (size_t i = 0; i < 4; ++i) {
    bytes.at(offset + i) = static_cast<uint8_t>(value >> (24 - 8 * i));
  }
}

// Offset of a node's child count, when its payload is empty.
constexpr size_t kChildCountOffset = 32 + 4;

std::vector<AuthTokenNode>
sample_nodes()
{
  return {
      node(1, 0),
      node(1, 1),
      node(1, 3),
      node(1, 4),
      node(1, 255),
      node(1, kAuthTokenMaxLen),
      node(1, 2, {node(2, 0), node(3, 5), node(4, 8), node(5, 13)}),
      node(1, 7, {node(2, 1, {node(3, 2), node(4, 3)}), node(5, 0)}),
      chain(kDepth),
  };
}

} // namespace

TEST(AuthSubtokenTest, DecodesValidNodesLikeAuthTokenNode)
{
  for (auto const &n : sample_nodes()) {
    Bytes bytes = encode(n);
    EXPECT_EQ(decode_as<Rc<AuthSubtoken>>(bytes, kDepth), bytes);
    expect_same_node_decode(bytes);
  }
}

TEST(AuthSubtokenTest, DecodesValidTokensLikeAuthToken)
{
  for (auto const &n : sample_nodes()) {
    Bytes bytes = encode(token(n));
    EXPECT_EQ(decode_as<RegisteredAuthToken>(bytes, kDepth), bytes);
    expect_same_token_decode(bytes);
  }
}

TEST(AuthSubtokenTest, DecodesTheTree)
{
  RegisteredAuthToken decoded;
  x23::deserialize(&decoded,
                   x23::serialize(token(
                       node(1, 2, {node(2, 3, {node(4, 0)}), node(3, 1)}))),
                   kDepth);

  EXPECT_EQ(decoded.mint_address, address(0xee));
  EXPECT_EQ(decoded.expiry_height, 1234);
  auto const &root = *decoded.root_node;
  EXPECT_EQ(root.consumer_address, address(1));
  EXPECT_EQ(root.payload, AuthTokenPayload(2, 1));
  ASSERT_EQ(root.child_nodes.size(), 2U);
  auto const &child = *root.child_nodes[0];
  EXPECT_EQ(child.consumer_address, address(2));
  EXPECT_EQ(child.payload, AuthTokenPayload(3, 2));
  ASSERT_EQ(child.child_nodes.size(), 1U);
  EXPECT_EQ(child.child_nodes[0]->consumer_address, address(4));
  EXPECT_TRUE(child.child_nodes[0]->child_nodes.empty());
  EXPECT_EQ(root.child_nodes[1]->consumer_address, address(3));
  EXPECT_TRUE(root.child_nodes[1]->child_nodes.empty());
}

TEST(AuthSubtokenTest, RejectsWhatAuthTokenNodeRejects)
{
  // Deeper than the limit, as a bare node and as a token's root.
  for (size_t limit : {1U, 2U, 4U, 8U}) {
    EXPECT_TRUE(
        decode_as<AuthTokenNode>(encode(chain(limit)), limit).has_value());
    EXPECT_FALSE(
        decode_as<AuthTokenNode>(encode(chain(limit + 1)), limit).has_value());
    expect_same_node_decode(encode(chain(limit)), limit);
    expect_same_node_decode(encode(chain(limit + 1)), limit);
    EXPECT_TRUE(
        decode_as<AuthToken>(encode(token(chain(limit))), limit).has_value());
    EXPECT_FALSE(decode_as<AuthToken>(encode(token(chain(limit + 1))), limit)
                     .has_value());
    expect_same_token_decode(encode(token(chain(limit))), limit);
    expect_same_token_decode(encode(token(chain(limit + 1))), limit);
  }

  // More children than kAuthTokenMaxChildNodes.
  Bytes five_children =
      encode(node(1, 0, {node(2, 0), node(3, 0), node(4, 0), node(5, 0)}));
  put32(five_children, kChildCountOffset, 5);
  five_children.append_range(encode(node(6, 0)));
  EXPECT_FALSE(decode_as<AuthTokenNode>(five_children, kDepth).has_value());
  expect_same_node_decode(five_children);

  Bytes huge_count = encode(node(1, 0));
  put32(huge_count, kChildCountOffset, 0xffffffff);
  expect_same_node_decode(huge_count);

  // A payload longer than kAuthTokenMaxLen.
  Bytes long_payload = encode(node(1, kAuthTokenMaxLen));
  put32(long_payload, 32, kAuthTokenMaxLen + 1);
  long_payload.insert(long_payload.begin() + 36 + kAuthTokenMaxLen, 4, 0);
  EXPECT_FALSE(decode_as<AuthTokenNode>(long_payload, kDepth).has_value());
  expect_same_node_decode(long_payload);

  // Nonzero padding after a payload.
  Bytes padding = encode(node(1, 1));
  padding.at(32 + 4 + 1) = 1;
  EXPECT_FALSE(decode_as<AuthTokenNode>(padding, kDepth).has_value());
  expect_same_node_decode(padding);

  // Trailing bytes, and every truncation.
  for (auto const &n : sample_nodes()) {
    for (Bytes const &bytes : {encode(n), encode(token(n))}) {
      Bytes trailing = bytes;
      trailing.insert(trailing.end(), 4, 0);
      expect_same_node_decode(trailing);
      expect_same_token_decode(trailing);
      for (size_t len = 0; len < bytes.size(); ++len) {
        Bytes truncated(bytes.begin(), bytes.begin() + len);
        expect_same_node_decode(truncated);
        expect_same_token_decode(truncated);
      }
    }
  }
}

TEST(AuthSubtokenTest, DecodesMutatedInputsLikeTheGeneratedTypes)
{
  std::mt19937 rng(4363);
  auto const nodes = sample_nodes();
  size_t accepted = 0;
  for (int i = 0; i < 20'000; ++i) {
    auto const &n = nodes[rng() % nodes.size()];
    bool as_token = rng() % 2 != 0;
    Bytes bytes = as_token ? encode(token(n)) : encode(n);
    for (int k = (rng() % 3) + 1; k > 0 && !bytes.empty(); --k) {
      size_t at = rng() % bytes.size();
      if (rng() % 2) {
        bytes[at] = static_cast<uint8_t>(rng());
      }
      else if ((at / 4 * 4) + 4 <= bytes.size()) {
        // A small value in a whole word, as a length or count would hold.
        put32(bytes, at / 4 * 4, rng() % 8);
      }
    }
    if (rng() % 4 == 0) {
      bytes.resize(rng() % (bytes.size() + 1));
    }
    if (as_token) {
      if (decode_as<AuthToken>(bytes, kDepth).has_value()) {
        ++accepted;
      }
      expect_same_token_decode(bytes);
    }
    else {
      if (decode_as<AuthTokenNode>(bytes, kDepth).has_value()) {
        ++accepted;
      }
      expect_same_node_decode(bytes);
    }
  }
  // Enough mutations decode for the comparison to cover accepted inputs too.
  EXPECT_GT(accepted, 1'000U);
}

TEST(AuthSubtokenTest, CopiesShareChildren)
{
  auto root = decode_node(node(1, 0, {node(2, 0), node(3, 0)}));
  auto copy = root->child_nodes;
  ASSERT_EQ(copy.size(), 2U);
  for (size_t i = 0; i < copy.size(); ++i) {
    EXPECT_EQ(copy[i].get(), root->child_nodes[i].get());
    EXPECT_EQ(copy[i].use_count(), 2U);
  }
}

} // namespace hogd
