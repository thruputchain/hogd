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

// Generates a random path with anywhere from 0 to 40 hex digits (inclusive).
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

void
ForEachFunc(std::map<HexArray, std::string> &seen,
            const std::pair<HexArray, std::string> &pair)
{
  seen[pair.first] = pair.second;
}

} // namespace

// These tests add an empty key to the trie.
TEST(MerkleTrie, EmptyKey)
{
  MerkleTrie trie;
  std::cout << trie << std::endl;

  HexArray key;
  std::string value = "hello";
  EXPECT_TRUE(trie.Insert(key, value));
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, EmptyKeyFind)
{
  MerkleTrie trie;

  HexArray key;
  std::string value = "hello";
  EXPECT_TRUE(trie.Insert(key, value));
  EXPECT_EQ(*trie.Find(key), value);
}

TEST(MerkleTrie, EmptyKeyRemove)
{
  MerkleTrie trie;

  HexArray key;
  std::string value = "hello";
  EXPECT_TRUE(trie.Insert(key, value));
  std::cout << trie << std::endl;
  EXPECT_TRUE(trie.Remove(key));
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, EmptyKeyRemoveWithChild)
{
  MerkleTrie trie;

  HexArray key_a;
  std::string value_a = "hello";
  EXPECT_TRUE(trie.Insert(key_a, value_a));

  HexArray key_b = HexArray("1");
  std::string value_b = "hello again";
  EXPECT_TRUE(trie.Insert(key_b, value_b));

  std::cout << trie << std::endl;
  EXPECT_TRUE(trie.Remove(key_a));
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, EmptyKeyRemoveChild)
{
  MerkleTrie trie;

  HexArray key_a;
  std::string value_a = "hello";
  EXPECT_TRUE(trie.Insert(key_a, value_a));

  HexArray key_b = HexArray("1");
  std::string value_b = "hello again";
  EXPECT_TRUE(trie.Insert(key_b, value_b));

  std::cout << trie << std::endl;
  EXPECT_TRUE(trie.Remove(key_b));
  std::cout << trie << std::endl;
}

// These tests add an empty value to the trie.
TEST(MerkleTrie, EmptyValue)
{
  MerkleTrie trie;
  std::cout << trie << std::endl;

  HexArray key("1");
  std::string value;
  EXPECT_TRUE(trie.Insert(key, value));
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, EmptyValueFind)
{
  MerkleTrie trie;

  HexArray key("1");
  std::string value;
  EXPECT_TRUE(trie.Insert(key, value));
  EXPECT_EQ(*trie.Find(key), value);
}

TEST(MerkleTrie, EmptyValueRemove)
{
  MerkleTrie trie;

  HexArray key("1");
  std::string value;
  EXPECT_TRUE(trie.Insert(key, value));
  std::cout << trie << std::endl;
  EXPECT_TRUE(trie.Remove(key));
  std::cout << trie << std::endl;
}

// These tests add keys of even and/or odd size to the trie.
TEST(MerkleTrie, EvenSizeKey)
{
  MerkleTrie trie;

  HexArray key("01");
  std::string value = "hello";
  EXPECT_TRUE(trie.Insert(key, value));
  EXPECT_EQ(*trie.Find(key), value);
}

TEST(MerkleTrie, OddSizeKey)
{
  MerkleTrie trie;

  HexArray key("012");
  std::string value = "hello";
  EXPECT_TRUE(trie.Insert(key, value));
  EXPECT_EQ(*trie.Find(key), value);
}

TEST(MerkleTrie, EvenAndOddSizedKeys)
{
  MerkleTrie trie;

  HexArray even_key("01");
  std::string even_value = "hello";
  EXPECT_TRUE(trie.Insert(even_key, even_value));

  HexArray odd_key("012");
  std::string odd_value = "hello";
  EXPECT_TRUE(trie.Insert(odd_key, odd_value));

  EXPECT_EQ(*trie.Find(even_key), even_value);
  EXPECT_EQ(*trie.Find(odd_key), odd_value);
}

// This tests that [Insert()] fails when attempting to insert a duplicate key.
TEST(MerkleTrie, InsertDuplicateKey)
{
  MerkleTrie trie;

  HexArray key("1234");
  std::string value = "hello";
  EXPECT_TRUE(trie.Insert(key, value));

  // Attempt to insert the same key again.
  EXPECT_FALSE(trie.Insert(key, "hello again"));

  EXPECT_EQ(*trie.Find(key), value);
}

// This tests that [Remove()] fails when attempting to remove a key that is not
// in the trie.
TEST(MerkleTrie, RemoveMissingKey)
{
  MerkleTrie trie;

  HexArray key("1234");
  std::string value = "hello";
  EXPECT_TRUE(trie.Insert(key, value));
  EXPECT_TRUE(trie.Remove(key));

  // Attempt to remove the same key again.
  EXPECT_FALSE(trie.Remove(key));
}

// These tests add two KV pairs to the trie, with both KV pairs being in leaf
// nodes.
TEST(MerkleTrie, TwoLeaves)
{
  MerkleTrie trie;
  std::cout << trie << std::endl;

  HexArray key("1234");
  std::string value = "hello";
  EXPECT_TRUE(trie.Insert(key, value));
  std::cout << trie << std::endl;

  key = HexArray("1223");
  value = "hello again";
  EXPECT_TRUE(trie.Insert(key, value));
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, TwoLeavesFind)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  EXPECT_TRUE(trie.Insert(key_a, value_a));
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(trie.Find(HexArray("12")), nullptr);

  HexArray key_b("1223");
  std::string value_b = "hello again";
  EXPECT_TRUE(trie.Insert(key_b, value_b));
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
  EXPECT_TRUE(trie.Insert(key_a, value_a));

  HexArray key_b("1223");
  std::string value_b = "hello again";
  EXPECT_TRUE(trie.Insert(key_b, value_b));
  std::cout << trie << std::endl;
  EXPECT_TRUE(trie.Remove(key_b));
  std::cout << trie << std::endl;
}

// These tests add two KV pairs to the trie, with one KV pair being in a branch
// node and the other being in a leaf node below the branch.
TEST(MerkleTrie, BranchAndLeaf)
{
  MerkleTrie trie;
  std::cout << trie << std::endl;

  HexArray key("1234");
  std::string value = "hello";
  EXPECT_TRUE(trie.Insert(key, value));
  std::cout << trie << std::endl;

  key = HexArray("123456");
  value = "hello again";
  EXPECT_TRUE(trie.Insert(key, value));
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, BranchAndLeafRemove)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  EXPECT_TRUE(trie.Insert(key_a, value_a));

  HexArray key_b = HexArray("123456");
  std::string value_b = "hello again";
  EXPECT_TRUE(trie.Insert(key_b, value_b));

  std::cout << trie << std::endl;
  EXPECT_TRUE(trie.Remove(key_b));
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, BranchAndLeafFind)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  EXPECT_TRUE(trie.Insert(key_a, value_a));
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(trie.Find(HexArray("12")), nullptr);

  HexArray key_b("123456");
  std::string value_b = "hello again";
  EXPECT_TRUE(trie.Insert(key_b, value_b));
  std::cout << trie << std::endl;
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(*trie.Find(key_b), value_b);
  EXPECT_EQ(trie.Find(HexArray("12")), nullptr);
  EXPECT_EQ(trie.Find(HexArray("12345678")), nullptr);
}

// This tests that a branch node with an empty value (i.e., a value of
// [std::nullopt]) is not mistakenly thought to contain a KV pair.
TEST(MerkleTrie, DoNotFindBranchValue)
{
  MerkleTrie trie;

  HexArray key_a("123456");
  std::string value_a = "hello";
  EXPECT_TRUE(trie.Insert(key_a, value_a));

  HexArray key_b("123478");
  std::string value_b = "hello again";
  EXPECT_TRUE(trie.Insert(key_b, value_b));

  // "1234" maps to a branch node that has no value (i.e., [node->value] is
  // [std::nullopt]). Check that we can properly detect this and return
  // [nullptr] rather than mistakenly return an empty string or something
  // similar.
  EXPECT_EQ(trie.Find(HexArray("1234")), nullptr);
}

// This test adds three KV pairs to the trie, with this structure:
//
//    [Branch]
//       |
//    [Branch]
//     |    |
// [Leaf]  [Branch (with value)]
//                 |
//               [Leaf]
TEST(MerkleTrie, BranchBranchLeaf)
{
  MerkleTrie trie;

  HexArray key_a("76c772");
  std::string value_a = "hello";
  EXPECT_TRUE(trie.Insert(key_a, value_a));
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(trie.Find(HexArray("12")), nullptr);
  std::cout << trie << std::endl;

  HexArray key_b("7ead1e640fb4854117ef3f882338ae");
  std::string value_b = "hello again";
  EXPECT_TRUE(trie.Insert(key_b, value_b));
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(*trie.Find(key_b), value_b);
  std::cout << trie << std::endl;

  HexArray key_c("76");
  std::string value_c = "hello three";
  EXPECT_TRUE(trie.Insert(key_c, value_c));
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(*trie.Find(key_b), value_b);
  EXPECT_EQ(*trie.Find(key_c), value_c);
  std::cout << trie << std::endl;
}

// These tests add three KV pairs to the trie, with this structure:
//    [Branch]
//       |
//   [Extension]
//       |
//    [Branch]
//     |    |
// [Leaf] [Extension]
//             |
//         [Branch (with value)]
//                 |
//               [Leaf]
TEST(MerkleTrie, TwoLeavesAndOneBranch)
{
  MerkleTrie trie;
  std::cout << trie << std::endl;

  HexArray key("1234");
  std::string value = "hello";
  EXPECT_TRUE(trie.Insert(key, value));
  std::cout << trie << std::endl;

  key = HexArray("123456");
  value = "hello again";
  EXPECT_TRUE(trie.Insert(key, value));
  std::cout << trie << std::endl;

  key = HexArray("12ab");
  value = "hello three";
  EXPECT_TRUE(trie.Insert(key, value));
  std::cout << trie << std::endl;
}

TEST(MerkleTrie, TwoLeavesAndOneBranchFind)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  EXPECT_TRUE(trie.Insert(key_a, value_a));
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(trie.Find(HexArray("12")), nullptr);

  HexArray key_b("123456");
  std::string value_b = "hello again";
  EXPECT_TRUE(trie.Insert(key_b, value_b));
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(*trie.Find(key_b), value_b);
  EXPECT_EQ(trie.Find(HexArray("12")), nullptr);
  EXPECT_EQ(trie.Find(HexArray("12345678")), nullptr);

  HexArray key_c("12ab");
  std::string value_c = "hello three";
  EXPECT_TRUE(trie.Insert(key_c, value_c));
  EXPECT_EQ(*trie.Find(key_a), value_a);
  EXPECT_EQ(*trie.Find(key_b), value_b);
  EXPECT_EQ(*trie.Find(key_c), value_c);
  EXPECT_EQ(trie.Find(HexArray("12")), nullptr);
  EXPECT_EQ(trie.Find(HexArray("12abcd")), nullptr);
  EXPECT_EQ(trie.Find(HexArray("12345678")), nullptr);
}

TEST(MerkleTrie, TwoLeavesAndOneBranchForEach)
{
  MerkleTrie trie;
  std::map<HexArray, std::string> seen;
  auto f = [&seen](const std::pair<HexArray, std::string> &pair) {
    ForEachFunc(seen, pair);
  };

  HexArray key_a("1234");
  std::string value_a = "hello";
  EXPECT_TRUE(trie.Insert(key_a, value_a));
  trie.ForEach(f);
  std::map<HexArray, std::string> expected = {{key_a, value_a}};
  EXPECT_EQ(seen, expected);

  HexArray key_b("123456");
  std::string value_b = "hello again";
  EXPECT_TRUE(trie.Insert(key_b, value_b));
  seen.clear();
  expected.insert({{key_b, value_b}});
  trie.ForEach(f);
  EXPECT_EQ(seen, expected);

  HexArray key_c("12ab");
  std::string value_c = "hello three";
  EXPECT_TRUE(trie.Insert(key_c, value_c));
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
  EXPECT_TRUE(trie.Insert(key_a, value_a));

  HexArray key_b("123456");
  std::string value_b = "hello again";
  EXPECT_TRUE(trie.Insert(key_b, value_b));

  HexArray key_c("12ab");
  std::string value_c = "hello three";
  EXPECT_TRUE(trie.Insert(key_c, value_c));

  std::cout << trie << std::endl;
  EXPECT_TRUE(trie.Remove(key_b));
  std::cout << trie << std::endl;
}

// This tests generating a proof and verifying a proof.
TEST(MerkleTrie, Prove)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  EXPECT_TRUE(trie.Insert(key_a, value_a));

  HexArray key_b("123456");
  std::string value_b = "hello again";
  EXPECT_TRUE(trie.Insert(key_b, value_b));

  HexArray key_c("12ab");
  std::string value_c = "hello three";
  EXPECT_TRUE(trie.Insert(key_c, value_c));

  const auto &[root_hash, node_encodings] = trie.GetProof(key_b);
  const auto &[root_hash_by_node, node_encodings_by_node] =
      trie.GetProofByNode(key_b);
  EXPECT_EQ(root_hash, root_hash_by_node);

  // [GetProof()] should be equivalent to [GetProofByNode()] when the latter is
  // flattened from a 2D vector into a 1D vector.
  std::vector<std::byte> node_encodings_by_node_flat;
  for (const std::vector<std::byte> &node_encoding : node_encodings_by_node) {
    node_encodings_by_node_flat.insert(node_encodings_by_node_flat.end(),
                                       node_encoding.begin(),
                                       node_encoding.end());
  }
  EXPECT_EQ(node_encodings, node_encodings_by_node_flat);

  // [key_b/value_b] is in the trie, so we should be able to prove it.
  EXPECT_TRUE(MerkleTrie::Prove(root_hash, node_encodings, key_b, value_b));

  // None of these pairs are in the trie, so we should not be able to prove
  // them.
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, node_encodings, key_a, value_a));
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, node_encodings, key_b, value_c));
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, node_encodings, key_c, value_b));

  // Try an empty key and an empty value.
  EXPECT_FALSE(
      MerkleTrie::Prove(root_hash, node_encodings, HexArray(), value_b));
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, node_encodings, key_b, ""));

  // Try removing a hex digit from the key.
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, node_encodings,
                                 key_b.GetSubarray(1, key_b.size()), value_b));

  {
    // Try adding a hex digit to the key.
    HexArray key_b_copy = key_b;
    key_b_copy.Append(std::byte{0x00});
    EXPECT_FALSE(
        MerkleTrie::Prove(root_hash, node_encodings, key_b_copy, value_b));
  }

  // Omit the node encodings.
  EXPECT_FALSE(MerkleTrie::Prove(root_hash, {}, key_b, value_b));
  {
    // Remove a byte from the beginning of the node encodings.
    std::vector<std::byte> copy(node_encodings.begin() + 1,
                                node_encodings.end());
    EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
  }
  {
    // Remove a byte from the end of the node encodings.
    std::vector<std::byte> copy(node_encodings.begin(),
                                node_encodings.end() - 1);
    EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
  }

  {
    // Try an incorrect root hash.
    SHA256Hash different_root_hash;
    memset(&different_root_hash, 0, sizeof(different_root_hash));
    EXPECT_FALSE(
        MerkleTrie::Prove(different_root_hash, node_encodings, key_b, value_b));
  }

  {
    // Try using the hash of an internal node as the root hash.
    SHA256Hash different_root_hash = sha256(node_encodings_by_node[1]);
    EXPECT_FALSE(
        MerkleTrie::Prove(different_root_hash, node_encodings, key_b, value_b));
  }

  {
    // Remove the leaf.
    std::vector<std::byte> copy(node_encodings.begin() +
                                    node_encodings_by_node[0].size(),
                                node_encodings.end());
    EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
  }

  {
    // Remove the root.
    std::vector<std::byte> copy(node_encodings.begin(),
                                node_encodings.end() -
                                    node_encodings_by_node.back().size());
    EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
  }

  {
    // Remove an intermediate node.
    std::vector<std::byte> copy(node_encodings.begin(),
                                node_encodings.begin() +
                                    node_encodings_by_node[0].size());
    copy.insert(copy.end(),
                node_encodings.begin() + node_encodings_by_node[0].size() +
                    node_encodings_by_node[1].size(),
                node_encodings.end());
    EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
  }

  {
    // Remove a byte from an intermediate node and test that the improper
    // encoding is detected.
    std::vector<std::byte> copy = node_encodings;
    copy.erase(copy.begin() + node_encodings_by_node[0].size());
    EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
  }

  {
    // Change the path in the leaf node.
    std::vector<std::byte> copy = node_encodings;
    copy[1 + sizeof(size_t)] = std::byte{0x00};
    EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
  }

  {
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
    {
      // Change the path in an extension node.
      std::vector<std::byte> copy = node_encodings;
      copy[idx + 1 + sizeof(size_t)] = std::byte{0x00};
      EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
    }

    {
      // Change the child hash in an extension node.
      size_t end_idx = node_encodings_by_node[node_idx].size();
      std::vector<std::byte> copy = node_encodings;
      uint8_t result = static_cast<uint8_t>(copy[end_idx - 1]) + 1;
      copy[end_idx - 1] = static_cast<std::byte>(result);
      EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
    }
  }

  {
    // Change the value in a leaf.
    size_t end_idx = node_encodings_by_node[0].size();
    std::vector<std::byte> copy = node_encodings;
    uint8_t result = static_cast<uint8_t>(copy[end_idx - 1]) + 1;
    copy[end_idx - 1] = static_cast<std::byte>(result);
    EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
  }

  {
    int matching_idx = -1;
    int nonmatching_idx;
    size_t branch_start_idx;
    {
      // Change the matching hash in a branch. [Prove()] should verify that the
      // hash in a branch's child hash array matches the hash in the child node
      // itself. This test changes the hash in the branch's child hash array
      // which should cause [Prove()] to return false.
      size_t idx = 0;
      bool found_branch = false;
      size_t node_idx;
      for (node_idx = 0; node_idx < node_encodings_by_node.size(); node_idx++) {
        if (static_cast<Node::Type>(node_encodings[idx]) ==
            Node::Type::kBranch) {
          found_branch = true;
          break;
        }
        else {
          idx += node_encodings_by_node[node_idx].size();
        }
      }
      ASSERT_TRUE(found_branch);
      std::vector<std::byte> copy = node_encodings;
      // Find matching branch.
      branch_start_idx = idx + node_encodings_by_node[node_idx].size() -
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
    }

    {
      // Change a non-matching hash in a branch. We will just choose a different
      // branch from above. This will cause the hash we generate for the branch
      // node to change, which should cause [Prove()] to return false.
      nonmatching_idx = matching_idx + sizeof(SHA256Hash);
      nonmatching_idx %= Node::kMaxNumBranches * sizeof(SHA256Hash);
      std::vector<std::byte> copy = node_encodings;

      {
        std::byte &change =
            copy[branch_start_idx + (nonmatching_idx * sizeof(SHA256Hash))];
        uint8_t result = static_cast<uint8_t>(change) + 1;
        change = static_cast<std::byte>(result);
        EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
      }
    }

    {
      // Change the ordering of the nodes in a branch.
      std::vector<std::byte> copy = node_encodings;
      SHA256Hash &matching_hash =
          *reinterpret_cast<SHA256Hash *>(&copy[matching_idx]);
      SHA256Hash &nonmatching_hash =
          *reinterpret_cast<SHA256Hash *>(&copy[nonmatching_idx]);
      std::swap(matching_hash, nonmatching_hash);
      EXPECT_FALSE(MerkleTrie::Prove(root_hash, copy, key_b, value_b));
    }
  }
}

// This tests that when we change a random byte in the proof, [Prove()] returns
// false;
TEST(MerkleTrie, ProveRandom)
{
  static size_t constexpr kNumIterations = 10000;
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  EXPECT_TRUE(trie.Insert(key_a, value_a));

  HexArray key_b("123456");
  std::string value_b = "hello again";
  EXPECT_TRUE(trie.Insert(key_b, value_b));

  HexArray key_c("12ab");
  std::string value_c = "hello three";
  EXPECT_TRUE(trie.Insert(key_c, value_c));

  auto [root_hash, node_encodings] = trie.GetProof(key_b);
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<size_t> byte_idx_dist(0, node_encodings.size() -
                                                             1);
  for (size_t i = 0; i < kNumIterations; i++) {
    // Change a random byte in [node_encodings].
    size_t byte_idx = byte_idx_dist(gen);
    std::byte cur = node_encodings[byte_idx];
    node_encodings[byte_idx] = std::byte(static_cast<char>(cur) + 0x1);

    EXPECT_FALSE(MerkleTrie::Prove(root_hash, node_encodings, key_b, value_b));
    node_encodings[byte_idx] = cur;
  }
}

// This test adds three KV pairs to the trie, with this structure:
//    [Branch]
//       |
//   [Extension]
//       |
//    [Branch]  ([key_c/value_c] should be placed in this node.)
//       |
//    [Branch]
//     |    |
// [Leaf]  [Leaf]
TEST(MerkleTrie, PlaceInBranchBelowExtensionNode)
{
  MerkleTrie trie;

  HexArray key_a("2671291b5a3887d332b73ec01345d7");
  std::string value_a = "hello";
  EXPECT_TRUE(trie.Insert(key_a, value_a));

  HexArray key_b("267124564f6835");
  std::string value_b = "hello again";
  EXPECT_TRUE(trie.Insert(key_b, value_b));
  std::cout << trie << std::endl;

  // This needs to be placed directly into the branch under the extension node.
  HexArray key_c("2671");
  std::string value_c = "hello three";
  EXPECT_TRUE(trie.Insert(key_c, value_c));
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

// This test stresses the Merkle trie. It randomly generates 10,000 different
// keys, places them into the trie, and then removes them from the trie. After
// each operation, this test checks that [ForEach()],
// [GetProof()/GetProofByNode()], [Prove()], and [Find()] return expected
// values. This test also performs an integrity check on the trie after each
// operation with [Check()].
TEST(MerkleTrie, Stress)
{
  static constexpr size_t kNumIterations = 10000;
  MerkleTrie trie;

  std::map<HexArray, std::string> total;
  std::map<HexArray, std::string> seen;
  auto f = [&seen](const std::pair<HexArray, std::string> &pair) {
    ForEachFunc(seen, pair);
  };

  // Insert the KV pairs.
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

  // Check every single KV pair.
  for (const auto &[path, value] : total) {
    const auto &[root_hash, node_encodings] = trie.GetProof(path);
    EXPECT_TRUE(MerkleTrie::Prove(root_hash, node_encodings, path, value));

    EXPECT_EQ(*trie.Find(path), value);
  }

  // Remove all KV pairs.
  for (size_t i = 0; i < kNumIterations; i++) {
    const auto &[path, value] = *total.begin();
    const auto &[root_hash, node_encodings] = trie.GetProof(path);
    EXPECT_TRUE(MerkleTrie::Prove(root_hash, node_encodings, path, value));

    EXPECT_TRUE(trie.Remove(path));
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
