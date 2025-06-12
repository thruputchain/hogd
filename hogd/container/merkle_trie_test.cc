#include <gtest/gtest.h>
#include <string.h>

#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hogd/container/merkle_trie.h"

namespace hogd {

namespace {

// Converts a human-readable hex string (e.g., "0x1234") to a [SHA256Hex].
SHA256Hash
HashFromString(const std::string &str)
{
  // A SHA-256 hash is 32 bytes, so [str] should have 32 * 2 = 64
  // characters.
  assert(str.size() == 64);

  SHA256Hash ret;
  for (size_t i = 0; i < str.size(); i += 2) {
    std::string substr = str.substr(i, 2);
    unsigned char byte = static_cast<unsigned char>(
        std::stoul(substr, /*pos=*/nullptr, /*base=*/16));
    ret[i / 2] = byte;
  }
  return ret;
}

std::vector<std::byte>
ByteVectorFromString(const std::string &str)
{
  // Each byte has two hex characters, so the string length must be even.
  assert(str.size() % 2 == 0);

  std::vector<std::byte> ret;
  for (size_t i = 0; i < str.size(); i += 2) {
    std::string substr = str.substr(i, 2);
    std::byte b = static_cast<std::byte>(
        std::stoul(substr, /*pos=*/nullptr, /*base=*/16));
    ret.push_back(b);
  }
  return ret;
}

} // namespace

// TODO: Test for empty branch values.

TEST(MerkleTrie, TwoLeaves)
{
  MerkleTrie trie;
  trie.Print();

  std::vector<std::byte> key = ByteVectorFromString("1234");
  std::string value = "hello";
  trie.insert(key, value);
  trie.Print();

  key = ByteVectorFromString("1223");
  value = "hello again";
  trie.insert(key, value);
  trie.Print();
}

TEST(MerkleTrie, TwoLeavesFind)
{
  MerkleTrie trie;

  std::vector<std::byte> key_a = ByteVectorFromString("1234");
  std::string value_a = "hello";
  trie.insert(key_a, value_a);
  EXPECT_EQ(*trie.find(key_a), value_a);
  EXPECT_EQ(trie.find(ByteVectorFromString("12")), nullptr);

  std::vector<std::byte> key_b = ByteVectorFromString("1223");
  std::string value_b = "hello again";
  trie.insert(key_b, value_b);
  EXPECT_EQ(*trie.find(key_a), value_a);
  EXPECT_EQ(*trie.find(key_b), value_b);
  EXPECT_EQ(trie.find(ByteVectorFromString("12")), nullptr);
  EXPECT_EQ(trie.find(ByteVectorFromString("123456")), nullptr);
}

TEST(MerkleTrie, BranchAndLeaf)
{
  MerkleTrie trie;
  trie.Print();

  std::vector<std::byte> key = ByteVectorFromString("1234");
  std::string value = "hello";
  trie.insert(key, value);
  trie.Print();

  key = ByteVectorFromString("123456");
  value = "hello again";
  trie.insert(key, value);
  trie.Print();
}

TEST(MerkleTrie, BranchAndLeafFind)
{
  MerkleTrie trie;

  std::vector<std::byte> key_a = ByteVectorFromString("1234");
  std::string value_a = "hello";
  trie.insert(key_a, value_a);
  EXPECT_EQ(*trie.find(key_a), value_a);
  EXPECT_EQ(trie.find(ByteVectorFromString("12")), nullptr);

  std::vector<std::byte> key_b = ByteVectorFromString("123456");
  std::string value_b = "hello again";
  trie.insert(key_b, value_b);
  EXPECT_EQ(*trie.find(key_a), value_a);
  EXPECT_EQ(*trie.find(key_b), value_b);
  EXPECT_EQ(trie.find(ByteVectorFromString("12")), nullptr);
  EXPECT_EQ(trie.find(ByteVectorFromString("12345678")), nullptr);
}

TEST(MerkleTrie, TwoLeavesAndOneBranch)
{
  MerkleTrie trie;
  trie.Print();

  std::vector<std::byte> key = ByteVectorFromString("1234");
  std::string value = "hello";
  trie.insert(key, value);
  trie.Print();

  key = ByteVectorFromString("123456");
  value = "hello again";
  trie.insert(key, value);
  trie.Print();

  key = ByteVectorFromString("12ab");
  value = "hello three";
  trie.insert(key, value);
  trie.Print();
}

TEST(MerkleTrie, TwoLeavesAndOneBranchFind)
{
  MerkleTrie trie;

  std::vector<std::byte> key_a = ByteVectorFromString("1234");
  std::string value_a = "hello";
  trie.insert(key_a, value_a);
  EXPECT_EQ(*trie.find(key_a), value_a);
  EXPECT_EQ(trie.find(ByteVectorFromString("12")), nullptr);

  std::vector<std::byte> key_b = ByteVectorFromString("123456");
  std::string value_b = "hello again";
  trie.insert(key_b, value_b);
  EXPECT_EQ(*trie.find(key_a), value_a);
  EXPECT_EQ(*trie.find(key_b), value_b);
  EXPECT_EQ(trie.find(ByteVectorFromString("12")), nullptr);
  EXPECT_EQ(trie.find(ByteVectorFromString("12345678")), nullptr);

  std::vector<std::byte> key_c = ByteVectorFromString("12ab");
  std::string value_c = "hello three";
  trie.insert(key_c, value_c);
  EXPECT_EQ(*trie.find(key_a), value_a);
  EXPECT_EQ(*trie.find(key_b), value_b);
  EXPECT_EQ(*trie.find(key_c), value_c);
  EXPECT_EQ(trie.find(ByteVectorFromString("12")), nullptr);
  EXPECT_EQ(trie.find(ByteVectorFromString("12abcd")), nullptr);
  EXPECT_EQ(trie.find(ByteVectorFromString("12345678")), nullptr);
}

// Hash a branch node. All child pointers are non-NULL.
TEST(MerkleTrie, HashBranch)
{
  MerkleTrie::Node branch;
  branch.type = MerkleTrie::NodeType::kBranch;
  memset(branch.hash.data(), 0, branch.hash.size() * sizeof(branch.hash[0]));
  branch.branch_value = "hello";
  std::vector<MerkleTrie::Node> children(16, MerkleTrie::Node{});
  for (size_t i = 0; i < children.size(); i++) {
    MerkleTrie::Node &child = children[i];
    size_t digit = i % 10;
    child.hash = HashFromString(std::string(64, '0' + digit));
    branch.branch_next[i] = &child;
  }

  SHA256Hash hash = MerkleTrie::GetNodeHash(branch);
  SHA256Hash expected = HashFromString(
      "8ed96e3ced26d6963836fa76f03874374c56bef15ae2c21b4acedcafa0ff47f0");
  EXPECT_EQ(hash, expected);
}

// Hash a branch node. Some child pointers are NULL and the rest are non-NULL.
TEST(MerkleTrie, HashBranchSomeNullptr)
{
  MerkleTrie::Node branch;
  branch.type = MerkleTrie::NodeType::kBranch;
  memset(branch.hash.data(), 0, branch.hash.size() * sizeof(branch.hash[0]));
  branch.branch_value = "hello";
  std::vector<MerkleTrie::Node> children(16, MerkleTrie::Node{});
  for (size_t i = 0; i < children.size(); i++) {
    if (i % 2) {
      MerkleTrie::Node &child = children[i];
      size_t digit = i % 10;
      child.hash = HashFromString(std::string(64, '0' + digit));
      branch.branch_next[i] = &child;
    }
    else {
      branch.branch_next[i] = nullptr;
    }
  }

  SHA256Hash hash = MerkleTrie::GetNodeHash(branch);
  SHA256Hash expected = HashFromString(
      "d40c498bbc37c10eb5c207db7645f6c7a0aaf04aae519bdc1b7491ed1d5bbabd");
  EXPECT_EQ(hash, expected);
}

// Hash a branch node. All child pointers are NULL.
TEST(MerkleTrie, HashBranchAllNullptr)
{
  MerkleTrie::Node branch;
  branch.type = MerkleTrie::NodeType::kBranch;
  memset(branch.hash.data(), 0, branch.hash.size() * sizeof(branch.hash[0]));
  branch.branch_value = "hello";
  memset(branch.branch_next.data(), 0,
         branch.branch_next.size() * sizeof(branch.branch_next[0]));

  SHA256Hash hash = MerkleTrie::GetNodeHash(branch);
  SHA256Hash expected = HashFromString(
      "57da3d5811ed733db97a0ac65af72d4d054dbd2435707633cc076f6dd153cc5e");
  EXPECT_EQ(hash, expected);
}

// Hash an extension node. Its child pointer is non-NULL.
TEST(MerkleTrie, HashExtension)
{
  MerkleTrie::Node extension;
  extension.type = MerkleTrie::NodeType::kExtension;
  memset(extension.hash.data(), 0,
         extension.hash.size() * sizeof(extension.hash[0]));
  extension.extension_path = {std::byte{0x00}, std::byte{0x01}, std::byte{0x02},
                              std::byte{0x03}};
  extension.extension_path_size = 8;

  static MerkleTrie::Node extension_key{
      .hash = HashFromString(
          "1111111111111111111111111111111111111111111111111111111111111111")};
  extension.extension_key = &extension_key;

  SHA256Hash hash = MerkleTrie::GetNodeHash(extension);
  SHA256Hash expected = HashFromString(
      "9b12e0b5041eb5015228a78c0fe2f1163d6037730d26cce2a94547ec4a793c9a");
  EXPECT_EQ(hash, expected);
}

// Hash an extension node. Its child pointer is NULL.
TEST(MerkleTrie, HashExtensionNullptr)
{
  MerkleTrie::Node extension;
  extension.type = MerkleTrie::NodeType::kExtension;
  memset(extension.hash.data(), 0,
         extension.hash.size() * sizeof(extension.hash[0]));
  extension.extension_path = {std::byte{0x00}, std::byte{0x01}, std::byte{0x02},
                              std::byte{0x03}};
  extension.extension_path_size = 8;

  extension.extension_key = nullptr;

  SHA256Hash hash = MerkleTrie::GetNodeHash(extension);
  SHA256Hash expected = HashFromString(
      "3461077c3e728e599bd45341f5db2a3e33595dcc74a495c7d5fdfdca5e964325");
  EXPECT_EQ(hash, expected);
}

// Hash a leaf node.
TEST(MerkleTrie, HashLeaf)
{
  MerkleTrie::Node leaf;
  leaf.type = MerkleTrie::NodeType::kLeaf;
  memset(leaf.hash.data(), 0, leaf.hash.size() * sizeof(leaf.hash[0]));
  leaf.leaf_path = {std::byte{0x00}, std::byte{0x01}, std::byte{0x02},
                    std::byte{0x03}};
  leaf.leaf_path_size = 8;
  leaf.leaf_value = "hello";

  SHA256Hash hash = MerkleTrie::GetNodeHash(leaf);
  SHA256Hash expected = HashFromString(
      "7224c1a06e40a068e8e904548fc399ee0c32263df98a113d5712f312789e3b36");
  EXPECT_EQ(hash, expected);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
