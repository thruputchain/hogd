#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
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

Bytes
encode(auto const &x)
{
  auto buf = x23::serialize(x);
  return {buf.begin(), buf.end()};
}

// What the generated AuthTokenNode decoder makes of bytes: the node,
// re-encoded, or nullopt if it rejects them.
std::optional<Bytes>
decode_as_node(Bytes const &bytes, size_t depth)
{
  AuthTokenNode n;
  try {
    x23::deserialize(&n, x23::BufView{std::as_bytes(std::span(bytes))}, depth);
  } catch (x23::XdrException const &) {
    return std::nullopt;
  }
  return encode(n);
}

// The same, for decode_auth_subtoken.
std::optional<Bytes>
decode_as_subtoken(Bytes const &bytes, size_t depth)
{
  Rc<AuthSubtoken> subtoken;
  try {
    subtoken = decode_auth_subtoken(std::as_bytes(std::span(bytes)), depth);
  } catch (x23::XdrException const &) {
    return std::nullopt;
  }
  return encode(*subtoken);
}

void
expect_same_decode(Bytes const &bytes, size_t depth = kDepth)
{
  EXPECT_EQ(decode_as_subtoken(bytes, depth), decode_as_node(bytes, depth))
      << "input of " << bytes.size() << " bytes";
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
sample_tokens()
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

TEST(AuthSubtokenTest, DecodesValidTokensLikeAuthTokenNode)
{
  for (auto const &token : sample_tokens()) {
    Bytes bytes = encode(token);
    EXPECT_EQ(decode_as_subtoken(bytes, kDepth), bytes);
    expect_same_decode(bytes);
  }
}

TEST(AuthSubtokenTest, DecodesTheTree)
{
  auto bytes = encode(node(1, 2, {node(2, 3, {node(4, 0)}), node(3, 1)}));
  auto root = decode_auth_subtoken(std::as_bytes(std::span(bytes)), kDepth);

  EXPECT_EQ(root->consumer_address, address(1));
  EXPECT_EQ(root->payload, AuthTokenPayload(2, 1));
  ASSERT_EQ(root->children.size(), 2U);
  EXPECT_EQ(root->children[0].consumer_address, address(2));
  EXPECT_EQ(root->children[0].payload, AuthTokenPayload(3, 2));
  ASSERT_EQ(root->children[0].children.size(), 1U);
  EXPECT_EQ(root->children[0].children[0].consumer_address, address(4));
  EXPECT_TRUE(root->children[0].children[0].children.empty());
  EXPECT_EQ(root->children[1].consumer_address, address(3));
  EXPECT_TRUE(root->children[1].children.empty());
}

TEST(AuthSubtokenTest, RejectsWhatAuthTokenNodeRejects)
{
  // Deeper than the limit.
  for (size_t limit : {1U, 2U, 4U, 8U}) {
    EXPECT_TRUE(decode_as_node(encode(chain(limit)), limit).has_value());
    expect_same_decode(encode(chain(limit)), limit);
    EXPECT_FALSE(decode_as_node(encode(chain(limit + 1)), limit).has_value());
    expect_same_decode(encode(chain(limit + 1)), limit);
  }

  // More children than kAuthTokenMaxChildNodes.
  Bytes five_children =
      encode(node(1, 0, {node(2, 0), node(3, 0), node(4, 0), node(5, 0)}));
  put32(five_children, kChildCountOffset, 5);
  five_children.append_range(encode(node(6, 0)));
  EXPECT_FALSE(decode_as_node(five_children, kDepth).has_value());
  expect_same_decode(five_children);

  Bytes huge_count = encode(node(1, 0));
  put32(huge_count, kChildCountOffset, 0xffffffff);
  expect_same_decode(huge_count);

  // A payload longer than kAuthTokenMaxLen.
  Bytes long_payload = encode(node(1, kAuthTokenMaxLen));
  put32(long_payload, 32, kAuthTokenMaxLen + 1);
  long_payload.insert(long_payload.begin() + 36 + kAuthTokenMaxLen, 4, 0);
  EXPECT_FALSE(decode_as_node(long_payload, kDepth).has_value());
  expect_same_decode(long_payload);

  // Nonzero padding after a payload.
  Bytes padding = encode(node(1, 1));
  padding.at(32 + 4 + 1) = 1;
  EXPECT_FALSE(decode_as_node(padding, kDepth).has_value());
  expect_same_decode(padding);

  // Trailing bytes, and every truncation.
  for (auto const &token : sample_tokens()) {
    Bytes bytes = encode(token);
    Bytes trailing = bytes;
    trailing.insert(trailing.end(), 4, 0);
    expect_same_decode(trailing);
    for (size_t len = 0; len < bytes.size(); ++len) {
      expect_same_decode(Bytes(bytes.begin(), bytes.begin() + len));
    }
  }
}

TEST(AuthSubtokenTest, DecodesMutatedInputsLikeAuthTokenNode)
{
  std::mt19937 rng(4363);
  auto const tokens = sample_tokens();
  size_t accepted = 0;
  for (int i = 0; i < 20'000; ++i) {
    Bytes bytes = encode(tokens[rng() % tokens.size()]);
    for (int n = (rng() % 3) + 1; n > 0 && !bytes.empty(); --n) {
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
    if (decode_as_node(bytes, kDepth).has_value()) {
      ++accepted;
    }
    expect_same_decode(bytes);
  }
  // Enough mutations decode for the comparison to cover accepted inputs too.
  EXPECT_GT(accepted, 1'000U);
}

TEST(AuthSubtokenTest, DecodesAuthTokenLikeAuthToken)
{
  AuthToken token{.mint_address = address(9),
                  .expiry_height = 1234,
                  .root_node = node(1, 3, {node(2, 1)})};
  Bytes bytes = encode(token);
  auto [mint_address, expiry_height, root] =
      decode_auth_token(std::as_bytes(std::span(bytes)), kDepth);
  EXPECT_EQ(mint_address, token.mint_address);
  EXPECT_EQ(expiry_height, token.expiry_height);
  EXPECT_EQ(encode(*root), encode(token.root_node));

  bytes.pop_back();
  EXPECT_THROW(decode_auth_token(std::as_bytes(std::span(bytes)), kDepth),
               x23::XdrException);
}

TEST(AuthSubtokenTest, CopiesShareChildren)
{
  auto root = make_auth_subtoken(node(1, 0, {node(2, 0), node(3, 0)}));
  AuthSubtokenChildren copy = root->children;
  ASSERT_EQ(copy.size(), 2U);
  for (size_t i = 0; i < copy.size(); ++i) {
    EXPECT_EQ(copy.nodes()[i].get(), root->children.nodes()[i].get());
    EXPECT_EQ(copy.nodes()[i].use_count(), 2U);
  }

  AuthSubtokenChildren moved = std::move(copy);
  EXPECT_EQ(moved.size(), 2U);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_TRUE(copy.empty());
}

} // namespace hogd
