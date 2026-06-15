#include <array>
#include <cstddef>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/allocator.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/node.h"
#include "hogd/crypto/crypto.h"
#include "types.hh"

namespace hogd {

namespace {

// Converts a human-readable hex string (e.g., "0x1234") to a [SHA256Hex].
SHA256Hash
hash_from_string(const std::string &str)
{
  // A SHA-256 hash is 32 bytes, so [str] should have 32 * 2 = 64
  // characters.
  CHECK_EQ(str.size(), 64UL);

  HexArray h(str);
  SHA256Hash ret;
  for (size_t i = 0; i < str.size(); i += 2) {
    std::byte first_hex_digit = h.get(i);
    std::byte second_hex_digit = h.get(i + 1);
    // [HexArray::get] always returns an [std::byte] with the most significant
    // four bits zeroed out. Thus, for [second_hex_digit] below, we do not need
    // to zero out these bits again.
    std::byte b = (first_hex_digit << 4) | second_hex_digit;
    ret[i / 2] = static_cast<unsigned char>(b);
  }
  return ret;
}

void
expect_hash_is_clear(const Node &node)
{
  for (unsigned char ch : node.hash) {
    EXPECT_EQ(ch, 0x00);
  }
}

void
expect_branch_children_are_clear(const Node &node)
{
  for (Node *child : node.branch_children()) {
    EXPECT_EQ(child, nullptr);
  }
}

void
expect_hash_matches_materialized_encoding(const Node &node)
{
  const std::vector<std::byte> &encoding = node.get_node_encoding();
  EXPECT_EQ(node.compute_node_hash(), sha256(encoding));
}

} // namespace

TEST(Node, Clear)
{
  Node node;
  node.init_branch(nullptr);
  node.hash[0] = 0x01;
  node.parent = &node; // We just want the [parent] pointer to be non-NULL.
  node.value_hash() = SHA256Hash();
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  node.value_hash()->at(0) = 0xFF;
  // Similarly, we just want a couple of the child pointers to be non-NULL.
  node.branch_children()[0] = &node;
  node.branch_children()[Node::kMaxNumBranches - 1] = &node;

  node.clear();

  // We do not check [node.type] because [node.clear()] keeps the node type
  // rather than clearing it.
  expect_hash_is_clear(node);
  EXPECT_EQ(node.parent, nullptr);
  EXPECT_EQ(node.type, Node::Type::kBranch);
  EXPECT_EQ(node.value_hash(), std::nullopt);
  expect_branch_children_are_clear(node);
}

TEST(Node, CreateBranch)
{
  Node parent;
  Allocator<Node> allocator(Allocator<Node>::Type::kDefault);
  Node *branch = Node::create_branch(&parent, allocator);

  EXPECT_EQ(branch->type, Node::Type::kBranch);
  EXPECT_EQ(branch->parent, &parent);

  expect_hash_is_clear(*branch);
  EXPECT_EQ(branch->value_hash(), std::nullopt);
  expect_branch_children_are_clear(*branch);

  delete branch;
}

TEST(Node, InitBranch)
{
  Node parent;
  Node branch;

  branch.clear();
  branch.init_branch(&parent);

  EXPECT_EQ(branch.type, Node::Type::kBranch);
  EXPECT_EQ(branch.parent, &parent);
}

TEST(Node, CreateExtension)
{
  Node parent;
  Allocator<Node> allocator(Allocator<Node>::Type::kDefault);
  Node *extension = Node::create_extension(&parent, allocator);

  EXPECT_EQ(extension->type, Node::Type::kExtension);
  EXPECT_EQ(extension->parent, &parent);

  expect_hash_is_clear(*extension);
  EXPECT_TRUE(extension->path().empty());
  EXPECT_EQ(extension->extension_child(), nullptr);

  delete extension;
}

TEST(Node, InitExtension)
{
  Node parent;
  Node extension;

  extension.clear();
  extension.init_extension(&parent);

  EXPECT_EQ(extension.type, Node::Type::kExtension);
  EXPECT_EQ(extension.parent, &parent);
}

TEST(Node, CreateLeaf)
{
  Node parent;
  Allocator<Node> allocator(Allocator<Node>::Type::kDefault);
  Node *leaf = Node::create_leaf(&parent, allocator);

  EXPECT_EQ(leaf->type, Node::Type::kLeaf);
  EXPECT_EQ(leaf->parent, &parent);

  expect_hash_is_clear(*leaf);
  EXPECT_TRUE(leaf->path().empty());
  EXPECT_EQ(leaf->value_hash(), std::nullopt);

  delete leaf;
}

TEST(Node, InitLeaf)
{
  Node parent;
  Node leaf;

  leaf.clear();
  leaf.init_leaf(&parent);

  EXPECT_EQ(leaf.type, Node::Type::kLeaf);
  EXPECT_EQ(leaf.parent, &parent);
}

// This test hashes a branch node. All child pointers are non-NULL.
TEST(Node, HashBranch)
{
  Node branch;
  branch.init_branch(nullptr);
  memset(branch.hash.data(), 0, branch.hash.size() * sizeof(branch.hash[0]));
  branch.value_hash() = SHA256Hash();
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  branch.value_hash()->at(0) = 0xFF;
  std::array<Node, Node::kMaxNumBranches> children;
  for (size_t i = 0; i < children.size(); i++) {
    Node &child = children[i];
    size_t digit = i % 10;
    child.hash = hash_from_string(std::string(64, '0' + digit));
    branch.branch_children()[i] = &child;
  }

  SHA256Hash hash = branch.compute_node_hash();
  SHA256Hash expected = hash_from_string(
      "b817711d9a331bc95daf80ed68be9056c1d186b2a312b39312b4e0eccf43111c");
  EXPECT_EQ(hash, expected);
  expect_hash_matches_materialized_encoding(branch);
}

// This test hashes a branch node. Some child pointers are NULL and the rest are
// non-NULL.
TEST(Node, HashBranchSomeNullptr)
{
  Node branch;
  branch.init_branch(nullptr);
  memset(branch.hash.data(), 0, branch.hash.size() * sizeof(branch.hash[0]));
  branch.value_hash() = SHA256Hash();
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  branch.value_hash()->at(0) = 0xFF;
  std::array<Node, Node::kMaxNumBranches> children;
  for (size_t i = 0; i < children.size(); i++) {
    if (i % 2) {
      Node &child = children[i];
      size_t digit = i % 10;
      child.hash = hash_from_string(std::string(64, '0' + digit));
      branch.branch_children()[i] = &child;
    }
    else {
      branch.branch_children()[i] = nullptr;
    }
  }

  SHA256Hash hash = branch.compute_node_hash();
  SHA256Hash expected = hash_from_string(
      "2d18826abe1140d6765485d7e1fb6ffd36d5806f4c8d30005a2a72fffa603553");
  EXPECT_EQ(hash, expected);
  expect_hash_matches_materialized_encoding(branch);
}

// This test hashes a branch node. All child pointers are NULL.
TEST(Node, HashBranchAllNullptr)
{
  Node branch;
  branch.init_branch(nullptr);
  memset(branch.hash.data(), 0, branch.hash.size() * sizeof(branch.hash[0]));
  branch.value_hash() = SHA256Hash();
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  branch.value_hash()->at(0) = 0xFF;
  branch.branch_children().fill(nullptr);

  SHA256Hash hash = branch.compute_node_hash();
  SHA256Hash expected = hash_from_string(
      "5584b1de3f5cf388c00a706619518b443a5cb6199cd0207b8b983068a584d89c");
  EXPECT_EQ(hash, expected);
  expect_hash_matches_materialized_encoding(branch);
}

// This test hashes an extension node. Its child pointer is non-NULL.
TEST(Node, HashExtension)
{
  Node extension;
  extension.init_extension(nullptr);
  memset(extension.hash.data(), 0,
         extension.hash.size() * sizeof(extension.hash[0]));
  extension.path() = HexArray("00010203");

  static Node child;
  child.hash = hash_from_string(
      "1111111111111111111111111111111111111111111111111111111111111111");
  extension.extension_child() = &child;

  SHA256Hash hash = extension.compute_node_hash();
  SHA256Hash expected = hash_from_string(
      "d2af4c9af613a07327407c683265444011144af1e1b1658a9ff4bf14acbe930a");
  EXPECT_EQ(hash, expected);
  expect_hash_matches_materialized_encoding(extension);
}

// This test hashes a leaf node.
TEST(Node, HashLeaf)
{
  Node leaf;
  leaf.init_leaf(nullptr);
  memset(leaf.hash.data(), 0, leaf.hash.size() * sizeof(leaf.hash[0]));
  leaf.path() = HexArray("00010203");
  leaf.value_hash() = SHA256Hash();
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  leaf.value_hash()->at(0) = 0xFF;

  SHA256Hash hash = leaf.compute_node_hash();
  SHA256Hash expected = hash_from_string(
      "7e7dac6030735a4abea721534ce38b97f63cf230bfef391e4328099d3bc24e3d");
  EXPECT_EQ(hash, expected);
  expect_hash_matches_materialized_encoding(leaf);
}

TEST(Node, HashLeafOddPathMatchesMaterializedEncoding)
{
  Node leaf;
  leaf.init_leaf(nullptr);
  memset(leaf.hash.data(), 0, leaf.hash.size() * sizeof(leaf.hash[0]));
  leaf.path() = HexArray("123");
  leaf.value_hash() = SHA256Hash();
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  leaf.value_hash()->at(0) = 0xAB;

  expect_hash_matches_materialized_encoding(leaf);
}

TEST(Node, ReinitAcrossNodeTypes)
{
  Node parent;
  Node child;
  Node node;

  node.init_extension(&parent);
  node.path() = HexArray("abcd");
  node.extension_child() = &child;

  node.init_branch(&parent);
  EXPECT_EQ(node.type, Node::Type::kBranch);
  EXPECT_EQ(node.parent, &parent);
  EXPECT_EQ(node.value_hash(), std::nullopt);
  expect_branch_children_are_clear(node);

  node.init_leaf(&parent);
  node.path() = HexArray("12");
  node.value_hash() = SHA256Hash();
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  node.value_hash()->at(0) = 0x5A;

  node.init_extension(&parent);
  EXPECT_EQ(node.type, Node::Type::kExtension);
  EXPECT_TRUE(node.path().empty());
  EXPECT_EQ(node.extension_child(), nullptr);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  // Initialize logging. This is needed for the CHECK macros.
  log_init(std::make_unique<hogd::ConsoleLogger>());

  return RUN_ALL_TESTS();
}
