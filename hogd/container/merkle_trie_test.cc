#include <gtest/gtest.h>
#include <string.h>

#include <atomic>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hogd/base/check.h"
#include "hogd/container/merkle_trie.h"
#include "hogd/container/node.h"

namespace hogd {

namespace {

// Converts a human-readable hex string (e.g., "0x1234") to a [SHA256Hex].
SHA256Hash
HashFromString(const std::string &str)
{
  // A SHA-256 hash is 32 bytes, so [str] should have 32 * 2 = 64
  // characters.
  CHECK_EQ(str.size(), 64);

  HexArray h(str);
  SHA256Hash ret;
  for (size_t i = 0; i < str.size(); i += 2) {
    std::byte first_hex_digit = h.Get(i);
    std::byte second_hex_digit = h.Get(i + 1);
    // [HexArray::Get] always return an [std::byte] with the most significant
    // four bits zeroed out. Thus, for [second_hex_digit] below, we do not need
    // to zero out these bits again.
    std::byte b = (first_hex_digit << 4) | second_hex_digit;
    ret[i / 2] = static_cast<unsigned char>(b);
  }
  return ret;
}

HexArray
GenerateRandomPath()
{
  std::random_device rd;
  std::mt19937 gen(rd());

  std::uniform_int_distribution<size_t> path_size_dist(0, 40);
  size_t path_size = path_size_dist(gen);

  // [path_size] may be odd, but [path] can only hold an even number of hex
  // digits because there are two hex digits in an [std::byte]. We remove the
  // extra hex digit below before returning the path.
  std::vector<std::byte> path((path_size + 1) / 2);
  std::uniform_int_distribution<int> path_dist(0, 0xff);

  for (size_t i = 0; i < path.size(); i++) {
    path[i] = static_cast<std::byte>(path_dist(gen));
  }
  HexArray ret(path);
  // If [path_size] is odd, remove a hex digit from the from path.
  if (path_size % 2) {
    ret.RemoveFromBack(1);
  }
  return ret;
}

} // namespace

TEST(MerkleTrie, EmptyKey)
{
  MerkleTrie trie;
  std::cout << trie << std::endl;

  HexArray key;
  std::string value = "hello";
  trie.Insert(key, value);
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, EmptyKeyFind)
{
  MerkleTrie trie;

  HexArray key;
  std::string value = "hello";
  trie.Insert(key, value);
  EXPECT_EQ(*trie.Find(key), value);
}

TEST(MerkleTrie, EmptyKeyRemove)
{
  MerkleTrie trie;

  HexArray key;
  std::string value = "hello";
  trie.Insert(key, value);
  std::cout << trie << std::endl;
  trie.Remove(key);
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, EmptyKeyRemoveWithChild)
{
  MerkleTrie trie;

  HexArray key_a;
  std::string value_a = "hello";
  trie.Insert(key_a, value_a);

  HexArray key_b = HexArray("1");
  std::string value_b = "hello again";
  trie.Insert(key_b, value_b);

  std::cout << trie << std::endl;
  trie.Remove(key_a);
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, EmptyKeyRemoveChild)
{
  MerkleTrie trie;

  HexArray key_a;
  std::string value_a = "hello";
  trie.Insert(key_a, value_a);

  HexArray key_b = HexArray("1");
  std::string value_b = "hello again";
  trie.Insert(key_b, value_b);

  std::cout << trie << std::endl;
  trie.Remove(key_b);
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, EmptyValue)
{
  MerkleTrie trie;
  std::cout << trie << std::endl;

  HexArray key("1");
  std::string value;
  trie.Insert(key, value);
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, EmptyValueFind)
{
  MerkleTrie trie;

  HexArray key("1");
  std::string value;
  trie.Insert(key, value);
  EXPECT_EQ(*trie.Find(key), value);
}

TEST(MerkleTrie, EmptyValueRemove)
{
  MerkleTrie trie;

  HexArray key("1");
  std::string value;
  trie.Insert(key, value);
  std::cout << trie << std::endl;
  trie.Remove(key);
  std::cout << trie << std::endl;
}

// TODO: Check the return value of [trie.Insert()].

TEST(MerkleTrie, TwoLeaves)
{
  MerkleTrie trie;
  std::cout << trie << std::endl;

  HexArray key("1234");
  std::string value = "hello";
  trie.Insert(key, value);
  std::cout << trie << std::endl;

  key = HexArray("1223");
  value = "hello again";
  trie.Insert(key, value);
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, TwoLeavesFind)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  trie.Insert(key_a, value_a);
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(trie.Find(HexArray("12")), nullptr);

  HexArray key_b("1223");
  std::string value_b = "hello again";
  trie.Insert(key_b, value_b);
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(*trie.Find(key_b), value_b);
  EXPECT_EQ(trie.Find(HexArray("12")), nullptr);
  EXPECT_EQ(trie.Find(HexArray("123456")), nullptr);
}

TEST(MerkleTrie, TwoLeavesRemove)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  trie.Insert(key_a, value_a);

  HexArray key_b("1223");
  std::string value_b = "hello again";
  trie.Insert(key_b, value_b);
  std::cout << trie << std::endl;
  trie.Remove(key_b);
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, BranchAndLeaf)
{
  MerkleTrie trie;
  std::cout << trie << std::endl;

  HexArray key("1234");
  std::string value = "hello";
  trie.Insert(key, value);
  std::cout << trie << std::endl;

  key = HexArray("123456");
  value = "hello again";
  trie.Insert(key, value);
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, BranchAndLeafRemove)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  trie.Insert(key_a, value_a);

  HexArray key_b = HexArray("123456");
  std::string value_b = "hello again";
  trie.Insert(key_b, value_b);

  std::cout << trie << std::endl;
  trie.Remove(key_b);
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, BranchAndLeafFind)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  trie.Insert(key_a, value_a);
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(trie.Find(HexArray("12")), nullptr);

  HexArray key_b("123456");
  std::string value_b = "hello again";
  trie.Insert(key_b, value_b);
  std::cout << trie << std::endl;
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(*trie.Find(key_b), value_b);
  EXPECT_EQ(trie.Find(HexArray("12")), nullptr);
  EXPECT_EQ(trie.Find(HexArray("12345678")), nullptr);
}

TEST(MerkleTrie, DoNotFindBranchValue)
{
  MerkleTrie trie;

  HexArray key_a("123456");
  std::string value_a = "hello";
  trie.Insert(key_a, value_a);

  HexArray key_b("123478");
  std::string value_b = "hello again";
  trie.Insert(key_b, value_b);

  // "1234" maps to a branch node that has no value (i.e., [node->value] is
  // [std::nullopt]). Check that we can properly detect this and return
  // [nullptr] rather than mistakenly return an empty string or something
  // similar.
  EXPECT_EQ(trie.Find(HexArray("1234")), nullptr);
}

TEST(MerkleTrie, BranchBranchLeaf)
{
  MerkleTrie trie;

  HexArray key_a("76c772");
  std::string value_a = "hello";
  trie.Insert(key_a, value_a);
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(trie.Find(HexArray("12")), nullptr);
  std::cout << trie << std::endl;

  HexArray key_b("7ead1e640fb4854117ef3f882338ae");
  std::string value_b = "hello again";
  trie.Insert(key_b, value_b);
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(*trie.Find(key_b), value_b);
  std::cout << trie << std::endl;

  HexArray key_c("76");
  std::string value_c = "hello three";
  trie.Insert(key_c, value_c);
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(*trie.Find(key_b), value_b);
  EXPECT_EQ(*trie.Find(key_c), value_c);
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, TwoLeavesAndOneBranch)
{
  MerkleTrie trie;
  std::cout << trie << std::endl;

  HexArray key("1234");
  std::string value = "hello";
  trie.Insert(key, value);
  std::cout << trie << std::endl;

  key = HexArray("123456");
  value = "hello again";
  trie.Insert(key, value);
  std::cout << trie << std::endl;

  key = HexArray("12ab");
  value = "hello three";
  trie.Insert(key, value);
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, TwoLeavesAndOneBranchFind)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  trie.Insert(key_a, value_a);
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(trie.Find(HexArray("12")), nullptr);

  HexArray key_b("123456");
  std::string value_b = "hello again";
  trie.Insert(key_b, value_b);
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(*trie.Find(key_b), value_b);
  EXPECT_EQ(trie.Find(HexArray("12")), nullptr);
  EXPECT_EQ(trie.Find(HexArray("12345678")), nullptr);

  HexArray key_c("12ab");
  std::string value_c = "hello three";
  trie.Insert(key_c, value_c);
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(*trie.Find(key_b), value_b);
  EXPECT_EQ(*trie.Find(key_c), value_c);
  EXPECT_EQ(trie.Find(HexArray("12")), nullptr);
  EXPECT_EQ(trie.Find(HexArray("12abcd")), nullptr);
  EXPECT_EQ(trie.Find(HexArray("12345678")), nullptr);
}

namespace {

void
ForEachFunc(std::map<HexArray, std::string> &seen,
            const std::pair<HexArray, std::string> &pair)
{
  seen[pair.first] = pair.second;
}

} // namespace

TEST(MerkleTrie, TwoLeavesAndOneBranchForEach)
{
  MerkleTrie trie;
  std::map<HexArray, std::string> seen;
  auto f = [&seen](const std::pair<HexArray, std::string> &pair) {
    ForEachFunc(seen, pair);
  };

  HexArray key_a("1234");
  std::string value_a = "hello";
  trie.Insert(key_a, value_a);
  trie.ForEach(f);
  std::map<HexArray, std::string> expected = {{key_a, value_a}};
  EXPECT_EQ(seen, expected);

  HexArray key_b("123456");
  std::string value_b = "hello again";
  trie.Insert(key_b, value_b);
  seen.clear();
  expected.insert({{key_b, value_b}});
  trie.ForEach(f);
  EXPECT_EQ(seen, expected);

  HexArray key_c("12ab");
  std::string value_c = "hello three";
  trie.Insert(key_c, value_c);
  seen.clear();
  expected.insert({{key_c, value_c}});
  trie.ForEach(f);
  EXPECT_EQ(seen, expected);
}

TEST(MerkleTrie, TwoLeavesAndOneBranchRemove)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  trie.Insert(key_a, value_a);

  HexArray key_b("123456");
  std::string value_b = "hello again";
  trie.Insert(key_b, value_b);

  HexArray key_c("12ab");
  std::string value_c = "hello three";
  trie.Insert(key_c, value_c);

  std::cout << trie << std::endl;
  trie.Remove(key_b);
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, TwoLeavesAndOneBranchProve)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  trie.Insert(key_a, value_a);

  HexArray key_b("123456");
  std::string value_b = "hello again";
  trie.Insert(key_b, value_b);

  HexArray key_c("12ab");
  std::string value_c = "hello three";
  trie.Insert(key_c, value_c);

  std::cout << trie << std::endl;
  const auto &[root_hash, node_encodings] = trie.GetProof(key_b);
  const auto &[root_hash_by_node, node_encodings_by_node] =
      trie.GetProofByNode(key_b);
  EXPECT_EQ(root_hash, root_hash_by_node);
  std::vector<std::byte> node_encodings_by_node_flat;
  for (const std::vector<std::byte> &node_encoding : node_encodings_by_node) {
    node_encodings_by_node_flat.insert(node_encodings_by_node_flat.end(),
                                       node_encoding.begin(),
                                       node_encoding.end());
  }
  EXPECT_EQ(node_encodings, node_encodings_by_node_flat);

  EXPECT_TRUE(MerkleTrie::Prove(root_hash, node_encodings, key_b, value_b));
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, node_encodings, key_a, value_a));
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, node_encodings, key_b, value_c));
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, node_encodings, key_c, value_b));

  EXPECT_FALSE(
      MerkleTrie::Prove(root_hash, node_encodings, HexArray(), value_b));
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, node_encodings, key_b, ""));
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, node_encodings,
                                 key_b.GetSubarray(1, key_b.size()), value_b));
  {
    HexArray key_b_copy = key_b;
    key_b_copy.Append(std::byte{0x00});
    EXPECT_FALSE(
        MerkleTrie::Prove(root_hash, node_encodings, key_b_copy, value_b));
  }

  EXPECT_FALSE(MerkleTrie::Prove(root_hash, {}, key_b, value_b));
  std::vector<std::byte> copy(node_encodings.begin() + 1, node_encodings.end());
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
  copy = std::vector(node_encodings.begin(), node_encodings.end() - 1);
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));

  // Try an incorrect root hash.
  {
    SHA256Hash different_root_hash;
    memset(&different_root_hash, 0, sizeof(different_root_hash));
    EXPECT_FALSE(MerkleTrie::Prove(different_root_hash, copy, key_b, value_b));
  }

  // Try using the hash of an internal node as the root hash.
  {
    SHA256Hash different_root_hash = sha256(node_encodings_by_node[1]);
    EXPECT_FALSE(MerkleTrie::Prove(different_root_hash, copy, key_b, value_b));
  }

  // Remove the leaf.
  copy = std::vector(node_encodings.begin() + node_encodings_by_node[0].size(),
                     node_encodings.end());
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));

  // Remove the root.
  copy =
      std::vector(node_encodings.begin(),
                  node_encodings.end() - node_encodings_by_node.back().size());
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));

  // Remove an intermediate node.
  copy = std::vector(node_encodings.begin(),
                     node_encodings.begin() + node_encodings_by_node[0].size());
  copy.insert(copy.end(),
              node_encodings.begin() + node_encodings_by_node[0].size() +
                  node_encodings_by_node[1].size(),
              node_encodings.end());
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));

  // Remove a byte from an intermediate node and test that the improper encoding
  // is detected.
  // TODO: Do this for all node types.
  copy = node_encodings;
  copy.erase(copy.begin() + node_encodings_by_node[0].size());
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));

  // Change the path in the leaf node.
  copy = node_encodings;
  copy[1 + sizeof(size_t)] = std::byte{0x00};
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));

  // Change the path in an extension node.
  size_t idx = 0;
  size_t node_idx = 0;
  bool found_extension = false;
  for (node_idx = 0; node_idx < node_encodings_by_node.size(); node_idx++) {
    if (static_cast<Node::Type>(node_encodings[idx]) ==
        Node::Type::kExtension) {
      found_extension = true;
      break;
    }
    else {
      idx += node_encodings_by_node[node_idx].size();
    }
  }
  ASSERT_TRUE(found_extension);
  copy = node_encodings;
  copy[idx + 1 + sizeof(size_t)] = std::byte{0x00};
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));

  // Change the child hash in an extension node.
  {
    size_t end_idx = node_encodings_by_node[node_idx].size();
    copy = node_encodings;
    uint8_t result = static_cast<uint8_t>(copy[end_idx - 1]) + 1;
    copy[end_idx - 1] = static_cast<std::byte>(result);
    EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
  }

  // Change the value in a leaf.
  {
    size_t end_idx = node_encodings_by_node[0].size();
    copy = node_encodings;
    uint8_t result = static_cast<uint8_t>(copy[end_idx - 1]) + 1;
    copy[end_idx - 1] = static_cast<std::byte>(result);
    EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
  }

  // Change the matching hash in a branch.
  idx = 0;
  bool found_branch = false;
  for (node_idx = 0; node_idx < node_encodings_by_node.size(); node_idx++) {
    if (static_cast<Node::Type>(node_encodings[idx]) == Node::Type::kBranch) {
      found_branch = true;
      break;
    }
    else {
      idx += node_encodings_by_node[node_idx].size();
    }
  }
  ASSERT_TRUE(found_branch);
  copy = node_encodings;
  // Find matching branch.
  int matching_idx = -1;
  size_t branch_start_idx = idx + node_encodings_by_node[node_idx].size() -
                            (Node::kMaxNumBranches * sizeof(SHA256Hash));
  SHA256Hash to_find = sha256(node_encodings_by_node[node_idx - 1]);
  for (size_t i = 0; i < Node::kMaxNumBranches; i++) {
    SHA256Hash *to_compare = reinterpret_cast<SHA256Hash *>(
        &copy[branch_start_idx + (i * sizeof(SHA256Hash))]);
    if (*to_compare == to_find) {
      matching_idx = i;
      break;
    }
  }
  ASSERT_GE(matching_idx, 0);
  {
    std::byte &change =
        copy[branch_start_idx + (matching_idx * sizeof(SHA256Hash))];
    uint8_t result = static_cast<uint8_t>(change) + 1;
    change = static_cast<std::byte>(result);
    EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
  }

  // Change a non-matching hash in a branch. We will just choose a different
  // branch from above.
  int nonmatching_idx = matching_idx + sizeof(SHA256Hash);
  nonmatching_idx %= Node::kMaxNumBranches * sizeof(SHA256Hash);
  copy = node_encodings;

  {
    std::byte &change =
        copy[branch_start_idx + (nonmatching_idx * sizeof(SHA256Hash))];
    uint8_t result = static_cast<uint8_t>(change) + 1;
    change = static_cast<std::byte>(result);
    EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
  }

  // Change the ordering of the nodes in a branch.
  copy = node_encodings;
  SHA256Hash &matching_hash =
      *reinterpret_cast<SHA256Hash *>(&copy[matching_idx]);
  SHA256Hash &nonmatching_hash =
      *reinterpret_cast<SHA256Hash *>(&copy[nonmatching_idx]);
  std::swap(matching_hash, nonmatching_hash);
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
}

TEST(MerkleTrie, PlaceInBranchBelowExtensionNode)
{
  MerkleTrie trie;

  HexArray key_a("2671291b5a3887d332b73ec01345d7");
  std::string value_a = "hello";
  trie.Insert(key_a, value_a);

  HexArray key_b("267124564f6835");
  std::string value_b = "hello again";
  trie.Insert(key_b, value_b);
  std::cout << trie << std::endl;

  // This needs to be placed directly into the branch under the extension node.
  HexArray key_c("2671");
  std::string value_c = "hello three";
  trie.Insert(key_c, value_c);
  std::cout << trie << std::endl;

  std::map<HexArray, std::string> seen;
  auto f = [&seen](const std::pair<HexArray, std::string> &pair) {
    ForEachFunc(seen, pair);
  };
  trie.ForEach(f);
  std::map<HexArray, std::string> expected = {{key_a, value_a},
                                              {key_b, value_b},
                                              {key_c, value_c}};
  EXPECT_EQ(seen, expected);
}

// Hash a branch node. All child pointers are non-NULL.
TEST(MerkleTrie, HashBranch)
{
  Node branch;
  branch.type = Node::Type::kBranch;
  memset(branch.hash.data(), 0, branch.hash.size() * sizeof(branch.hash[0]));
  branch.value = "hello";
  std::vector<Node> children(Node::kMaxNumBranches, Node{});
  for (size_t i = 0; i < children.size(); i++) {
    Node &child = children[i];
    size_t digit = i % 10;
    child.hash = HashFromString(std::string(64, '0' + digit));
    branch.child[i] = &child;
  }

  SHA256Hash hash = branch.ComputeNodeHash();
  SHA256Hash expected = HashFromString(
      "20b58e07d23b8c60f45291cfb51f3b266ac2e21afe18f31a1dbc22e76446c137");
  EXPECT_EQ(hash, expected);
}

// Hash a branch node. Some child pointers are NULL and the rest are non-NULL.
TEST(MerkleTrie, HashBranchSomeNullptr)
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
      child.hash = HashFromString(std::string(64, '0' + digit));
      branch.child[i] = &child;
    }
    else {
      branch.child[i] = nullptr;
    }
  }

  SHA256Hash hash = branch.ComputeNodeHash();
  SHA256Hash expected = HashFromString(
      "e237972b80de6f517ee566152092fef04a3d877adba234b7689420f80f2cdf46");
  EXPECT_EQ(hash, expected);
}

// Hash a branch node. All child pointers are NULL.
TEST(MerkleTrie, HashBranchAllNullptr)
{
  Node branch;
  branch.type = Node::Type::kBranch;
  memset(branch.hash.data(), 0, branch.hash.size() * sizeof(branch.hash[0]));
  branch.value = "hello";
  memset(branch.child.data(), 0, branch.child.size() * sizeof(branch.child[0]));

  SHA256Hash hash = branch.ComputeNodeHash();
  SHA256Hash expected = HashFromString(
      "e37f280ee5188cc892334bbfa5b456dc652c8d1c9b550a2fb8a9160095fe624e");
  EXPECT_EQ(hash, expected);
}

// Hash an extension node. Its child pointer is non-NULL.
TEST(MerkleTrie, HashExtension)
{
  Node extension;
  extension.type = Node::Type::kExtension;
  memset(extension.hash.data(), 0,
         extension.hash.size() * sizeof(extension.hash[0]));
  extension.path = HexArray("00010203");

  static Node child{
      .hash = HashFromString(
          "1111111111111111111111111111111111111111111111111111111111111111")};
  extension.child[0] = &child;

  SHA256Hash hash = extension.ComputeNodeHash();
  SHA256Hash expected = HashFromString(
      "d2af4c9af613a07327407c683265444011144af1e1b1658a9ff4bf14acbe930a");
  EXPECT_EQ(hash, expected);
}

// Hash a leaf node.
TEST(MerkleTrie, HashLeaf)
{
  Node leaf;
  leaf.type = Node::Type::kLeaf;
  memset(leaf.hash.data(), 0, leaf.hash.size() * sizeof(leaf.hash[0]));
  leaf.path = HexArray("00010203");
  leaf.value = "hello";

  SHA256Hash hash = leaf.ComputeNodeHash();
  SHA256Hash expected = HashFromString(
      "b8b1748d26a2919e0692c2bb970ccd1216b680f866c9a504bb173019ec1b0888");
  EXPECT_EQ(hash, expected);
}

TEST(MerkleTrie, Stress)
{
  static constexpr size_t kNumIterations = 10000;
  MerkleTrie trie;

  std::map<HexArray, std::string> total;
  std::map<HexArray, std::string> seen;
  auto f = [&seen](const std::pair<HexArray, std::string> &pair) {
    ForEachFunc(seen, pair);
  };

  for (size_t i = 0; i < kNumIterations; i++) {
    HexArray path = GenerateRandomPath();
    const std::string value = std::to_string(i);
    bool insert = trie.Insert(path, value);
    EXPECT_EQ(insert, total.count(path) == 0);
    if (!insert) {
      // A duplicate key was generated, so try again.
      i--;
      continue;
    }

    total.insert({path, value});

    seen.clear();
    trie.ForEach(f);
    EXPECT_EQ(seen, total);

    const auto &[root_hash, node_encodings] = trie.GetProof(path);
    const auto &[_, node_by_node] = trie.GetProofByNode(path);
    EXPECT_TRUE(MerkleTrie::Prove(root_hash, node_encodings, path, value));
    EXPECT_EQ(*trie.Find(path), value);

    EXPECT_TRUE(trie.Check());
  }

  for (const auto &[path, value] : total) {
    const auto &[root_hash, node_encodings] = trie.GetProof(path);
    EXPECT_TRUE(MerkleTrie::Prove(root_hash, node_encodings, path, value));

    EXPECT_EQ(*trie.Find(path), value);
  }

  for (size_t i = 0; i < kNumIterations; i++) {
    const auto &[path, value] = *total.begin();
    const auto &[root_hash, node_encodings] = trie.GetProof(path);
    EXPECT_TRUE(MerkleTrie::Prove(root_hash, node_encodings, path, value));

    EXPECT_EQ(trie.Remove(path), 1);
    auto it = total.erase(total.begin());
    if (i < kNumIterations - 1) {
      EXPECT_NE(it, total.end());
    }
    else {
      EXPECT_EQ(it, total.end());
    }

    EXPECT_EQ(trie.Find(path), nullptr);

    seen.clear();
    trie.ForEach(f);
    EXPECT_EQ(seen, total);

    EXPECT_TRUE(trie.Check());
  }
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
