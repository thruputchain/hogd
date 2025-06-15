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

} // namespace

// TODO: Test for empty branch values.

TEST(MerkleTrie, TwoLeaves)
{
  MerkleTrie trie;
  trie.Print();

  HexArray key("1234");
  std::string value = "hello";
  trie.insert(key, value);
  trie.Print();

  key = HexArray("1223");
  value = "hello again";
  trie.insert(key, value);
  trie.Print();
}

TEST(MerkleTrie, TwoLeavesFind)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  trie.insert(key_a, value_a);
  EXPECT_EQ(*trie.find(key_a), value_a);
  EXPECT_EQ(trie.find(HexArray("12")), nullptr);

  HexArray key_b("1223");
  std::string value_b = "hello again";
  trie.insert(key_b, value_b);
  EXPECT_EQ(*trie.find(key_a), value_a);
  EXPECT_EQ(*trie.find(key_b), value_b);
  EXPECT_EQ(trie.find(HexArray("12")), nullptr);
  EXPECT_EQ(trie.find(HexArray("123456")), nullptr);
}

TEST(MerkleTrie, TwoLeavesRemove)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  trie.insert(key_a, value_a);

  HexArray key_b("1223");
  std::string value_b = "hello again";
  trie.insert(key_b, value_b);
  trie.Print();
  trie.remove(key_b);
  trie.Print();
}

TEST(MerkleTrie, BranchAndLeaf)
{
  MerkleTrie trie;
  trie.Print();

  HexArray key("1234");
  std::string value = "hello";
  trie.insert(key, value);
  trie.Print();

  key = HexArray("123456");
  value = "hello again";
  trie.insert(key, value);
  trie.Print();
}

TEST(MerkleTrie, BranchAndLeafRemove)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  trie.insert(key_a, value_a);

  HexArray key_b = HexArray("123456");
  std::string value_b = "hello again";
  trie.insert(key_b, value_b);

  trie.Print();
  trie.remove(key_b);
  trie.Print();
}

TEST(MerkleTrie, BranchAndLeafFind)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  trie.insert(key_a, value_a);
  EXPECT_EQ(*trie.find(key_a), value_a);
  EXPECT_EQ(trie.find(HexArray("12")), nullptr);

  HexArray key_b("123456");
  std::string value_b = "hello again";
  trie.insert(key_b, value_b);
  EXPECT_EQ(*trie.find(key_a), value_a);
  EXPECT_EQ(*trie.find(key_b), value_b);
  EXPECT_EQ(trie.find(HexArray("12")), nullptr);
  EXPECT_EQ(trie.find(HexArray("12345678")), nullptr);
}

TEST(MerkleTrie, DoNotFindBranchValue)
{
  MerkleTrie trie;

  HexArray key_a("123456");
  std::string value_a = "hello";
  trie.insert(key_a, value_a);

  HexArray key_b("123478");
  std::string value_b = "hello again";
  trie.insert(key_b, value_b);

  // "1234" maps to a branch node that has no value (i.e., [node->value] is
  // [std::nullopt]). Check that we can properly detect this and return
  // [nullptr] rather than mistakenly return an empty string or something
  // similar.
  EXPECT_EQ(trie.find(HexArray("1234")), nullptr);
}

TEST(MerkleTrie, TwoLeavesAndOneBranch)
{
  MerkleTrie trie;
  trie.Print();

  HexArray key("1234");
  std::string value = "hello";
  trie.insert(key, value);
  trie.Print();

  key = HexArray("123456");
  value = "hello again";
  trie.insert(key, value);
  trie.Print();

  key = HexArray("12ab");
  value = "hello three";
  trie.insert(key, value);
  trie.Print();
}

TEST(MerkleTrie, TwoLeavesAndOneBranchFind)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  trie.insert(key_a, value_a);
  EXPECT_EQ(*trie.find(key_a), value_a);
  EXPECT_EQ(trie.find(HexArray("12")), nullptr);

  HexArray key_b("123456");
  std::string value_b = "hello again";
  trie.insert(key_b, value_b);
  EXPECT_EQ(*trie.find(key_a), value_a);
  EXPECT_EQ(*trie.find(key_b), value_b);
  EXPECT_EQ(trie.find(HexArray("12")), nullptr);
  EXPECT_EQ(trie.find(HexArray("12345678")), nullptr);

  HexArray key_c("12ab");
  std::string value_c = "hello three";
  trie.insert(key_c, value_c);
  EXPECT_EQ(*trie.find(key_a), value_a);
  EXPECT_EQ(*trie.find(key_b), value_b);
  EXPECT_EQ(*trie.find(key_c), value_c);
  EXPECT_EQ(trie.find(HexArray("12")), nullptr);
  EXPECT_EQ(trie.find(HexArray("12abcd")), nullptr);
  EXPECT_EQ(trie.find(HexArray("12345678")), nullptr);
}

namespace {

void
ForEachFunc(std::set<std::pair<HexArray, std::string>> &seen,
            const std::pair<HexArray, std::string> &pair)
{
  seen.insert(pair);
}

} // namespace

TEST(MerkleTrie, TwoLeavesAndOneBranchForEach)
{
  MerkleTrie trie;
  std::set<std::pair<HexArray, std::string>> seen;
  auto f = [&seen](const std::pair<HexArray, std::string> &pair) {
    ForEachFunc(seen, pair);
  };

  HexArray key_a("1234");
  std::string value_a = "hello";
  trie.insert(key_a, value_a);
  trie.for_each(f);
  std::set<std::pair<HexArray, std::string>> expected = {{key_a, value_a}};
  EXPECT_EQ(seen, expected);

  HexArray key_b("123456");
  std::string value_b = "hello again";
  trie.insert(key_b, value_b);
  seen.clear();
  expected.insert({{key_b, value_b}});
  trie.for_each(f);
  EXPECT_EQ(seen, expected);

  HexArray key_c("12ab");
  std::string value_c = "hello three";
  trie.insert(key_c, value_c);
  seen.clear();
  expected.insert({{key_c, value_c}});
  trie.for_each(f);
  EXPECT_EQ(seen, expected);
}

TEST(MerkleTrie, TwoLeavesAndOneBranchForRemove)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  trie.insert(key_a, value_a);

  HexArray key_b("123456");
  std::string value_b = "hello again";
  trie.insert(key_b, value_b);

  HexArray key_c("12ab");
  std::string value_c = "hello three";
  trie.insert(key_c, value_c);

  trie.Print();
  trie.remove(key_b);
  trie.Print();
}

// Hash a branch node. All child pointers are non-NULL.
TEST(MerkleTrie, HashBranch)
{
  MerkleTrie::Node branch;
  branch.type = MerkleTrie::NodeType::kBranch;
  memset(branch.hash.data(), 0, branch.hash.size() * sizeof(branch.hash[0]));
  branch.value = "hello";
  std::vector<MerkleTrie::Node> children(16, MerkleTrie::Node{});
  for (size_t i = 0; i < children.size(); i++) {
    MerkleTrie::Node &child = children[i];
    size_t digit = i % 10;
    child.hash = HashFromString(std::string(64, '0' + digit));
    branch.child[i] = &child;
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
  branch.value = "hello";
  std::vector<MerkleTrie::Node> children(16, MerkleTrie::Node{});
  for (size_t i = 0; i < children.size(); i++) {
    if (i % 2) {
      MerkleTrie::Node &child = children[i];
      size_t digit = i % 10;
      child.hash = HashFromString(std::string(64, '0' + digit));
      branch.child[i] = &child;
    }
    else {
      branch.child[i] = nullptr;
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
  branch.value = "hello";
  memset(branch.child.data(), 0, branch.child.size() * sizeof(branch.child[0]));

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
  extension.path = HexArray("00010203");

  static MerkleTrie::Node child{
      .hash = HashFromString(
          "1111111111111111111111111111111111111111111111111111111111111111")};
  extension.child[0] = &child;

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
  extension.path = HexArray("00010203");

  extension.child[0] = nullptr;

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
  leaf.path = HexArray("00010203");
  leaf.value = "hello";

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
