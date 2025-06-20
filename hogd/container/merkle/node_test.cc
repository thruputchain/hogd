#include <cstddef>
#include <cstring>
#include <gtest/gtest.h>
#include <optional>

#include <string>
#include <vector>

#include "hogd/base/check.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/node.h"
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
    // [HexArray::Get] always return an [std::byte] with the most significant
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
expect_children_are_clear(const Node &node)
{
  for (Node *child : node.child) {
    EXPECT_EQ(child, nullptr);
  }
}

} // namespace

TEST(Node, clear)
{
  Node node;
  // We do not set or check [node.type] because [node.type] is an enum class.
  // Setting it to a value of 0 is equivalent to setting it to [Node::kBranch],
  // which is an implementation detail.
  node.hash[0] = 0x01;
  node.parent = &node; // We just want the [parent] pointer to be non-NULL.
  node.path = HexArray("01");
  node.value = "hello";
  // Similarly, we just want a couple of the child pointers to be non-NULL.
  node.child[0] = &node;
  node.child[Node::kMaxNumBranches - 1] = &node;

  node.clear();

  // As mentioned above, we do not check [node.type].
  expect_hash_is_clear(node);
  EXPECT_EQ(node.parent, nullptr);
  EXPECT_TRUE(node.path.empty());
  EXPECT_EQ(node.value, std::nullopt);
  expect_children_are_clear(node);
}

TEST(Node, CreateBranch)
{
  Node parent;
  Node *branch = Node::create_branch(&parent);

  EXPECT_EQ(branch->type, Node::Type::kBranch);
  EXPECT_EQ(branch->parent, &parent);

  expect_hash_is_clear(*branch);
  EXPECT_TRUE(branch->path.empty());
  EXPECT_EQ(branch->value, std::nullopt);
  expect_children_are_clear(*branch);

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
  Node *extension = Node::create_extension(&parent);

  EXPECT_EQ(extension->type, Node::Type::kExtension);
  EXPECT_EQ(extension->parent, &parent);

  expect_hash_is_clear(*extension);
  EXPECT_TRUE(extension->path.empty());
  EXPECT_EQ(extension->value, std::nullopt);
  expect_children_are_clear(*extension);

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
  Node *leaf = Node::create_leaf(&parent);

  EXPECT_EQ(leaf->type, Node::Type::kLeaf);
  EXPECT_EQ(leaf->parent, &parent);

  expect_hash_is_clear(*leaf);
  EXPECT_TRUE(leaf->path.empty());
  EXPECT_EQ(leaf->value, std::nullopt);
  expect_children_are_clear(*leaf);

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
  branch.type = Node::Type::kBranch;
  memset(branch.hash.data(), 0, branch.hash.size() * sizeof(branch.hash[0]));
  branch.value = "hello";
  std::vector<Node> children(Node::kMaxNumBranches, Node{});
  for (size_t i = 0; i < children.size(); i++) {
    Node &child = children[i];
    size_t digit = i % 10;
    child.hash = hash_from_string(std::string(64, '0' + digit));
    branch.child[i] = &child;
  }

  SHA256Hash hash = branch.compute_node_hash();
  SHA256Hash expected = hash_from_string(
      "20b58e07d23b8c60f45291cfb51f3b266ac2e21afe18f31a1dbc22e76446c137");
  EXPECT_EQ(hash, expected);
}

// This test hashes a branch node. Some child pointers are NULL and the rest are
// non-NULL.
TEST(Node, HashBranchSomeNullptr)
{
  Node branch;
  branch.type = Node::Type::kBranch;
  memset(branch.hash.data(), 0, branch.hash.size() * sizeof(branch.hash[0]));
  branch.value = "hello";
  std::vector<Node> children(Node::kMaxNumBranches, Node{});
  for (size_t i = 0; i < children.size(); i++) {
    if (i % 2) {
      Node &child = children[i];
      size_t digit = i % 10;
      child.hash = hash_from_string(std::string(64, '0' + digit));
      branch.child[i] = &child;
    }
    else {
      branch.child[i] = nullptr;
    }
  }

  SHA256Hash hash = branch.compute_node_hash();
  SHA256Hash expected = hash_from_string(
      "e237972b80de6f517ee566152092fef04a3d877adba234b7689420f80f2cdf46");
  EXPECT_EQ(hash, expected);
}

// This test hashes a branch node. All child pointers are NULL.
TEST(Node, HashBranchAllNullptr)
{
  Node branch;
  branch.type = Node::Type::kBranch;
  memset(branch.hash.data(), 0, branch.hash.size() * sizeof(branch.hash[0]));
  branch.value = "hello";
  memset(static_cast<void *>(branch.child.data()), 0,
         branch.child.size() *
             sizeof(branch.child[0])); // NOLINT(bugprone-sizeof-expression)

  SHA256Hash hash = branch.compute_node_hash();
  SHA256Hash expected = hash_from_string(
      "e37f280ee5188cc892334bbfa5b456dc652c8d1c9b550a2fb8a9160095fe624e");
  EXPECT_EQ(hash, expected);
}

// This test hashes an extension node. Its child pointer is non-NULL.
TEST(Node, HashExtension)
{
  Node extension;
  extension.type = Node::Type::kExtension;
  memset(extension.hash.data(), 0,
         extension.hash.size() * sizeof(extension.hash[0]));
  extension.path = HexArray("00010203");

  static Node child{
      .hash = hash_from_string(
          "1111111111111111111111111111111111111111111111111111111111111111")};
  extension.child[0] = &child;

  SHA256Hash hash = extension.compute_node_hash();
  SHA256Hash expected = hash_from_string(
      "d2af4c9af613a07327407c683265444011144af1e1b1658a9ff4bf14acbe930a");
  EXPECT_EQ(hash, expected);
}

// This test hashes a leaf node.
TEST(Node, HashLeaf)
{
  Node leaf;
  leaf.type = Node::Type::kLeaf;
  memset(leaf.hash.data(), 0, leaf.hash.size() * sizeof(leaf.hash[0]));
  leaf.path = HexArray("00010203");
  leaf.value = "hello";

  SHA256Hash hash = leaf.compute_node_hash();
  SHA256Hash expected = hash_from_string(
      "b8b1748d26a2919e0692c2bb970ccd1216b680f866c9a504bb173019ec1b0888");
  EXPECT_EQ(hash, expected);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
