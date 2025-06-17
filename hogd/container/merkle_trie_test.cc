#include <gtest/gtest.h>
#include <string.h>

#include <atomic>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hogd/container/merkle_trie.h"

namespace hogd {

namespace {

// TODO: This functionality should be replaced with HexArray.

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

/*
std::string
StringFromHash(const SHA256Hash &hash)
{
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (size_t i = 0; i < 32; i++) {
    oss << std::setw(2) << static_cast<int>(hash[i]);
  }
  return oss.str();
}
*/

} // namespace

// TODO: Try to add an empty key. Try to add an empty value.

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
  printf("1a\n");
  EXPECT_EQ(*trie.find(key_a), value_a);
  printf("1b\n");
  EXPECT_EQ(trie.find(HexArray("12")), nullptr);

  HexArray key_b("123456");
  std::string value_b = "hello again";
  trie.insert(key_b, value_b);
  printf("1\n");
  EXPECT_EQ(*trie.find(key_a), value_a);
  printf("2\n");
  EXPECT_EQ(*trie.find(key_b), value_b);
  printf("3\n");
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

TEST(MerkleTrie, BranchBranchLeaf)
{
  MerkleTrie trie;

  HexArray key_a("76c772");
  std::string value_a = "hello";
  trie.insert(key_a, value_a);
  EXPECT_EQ(*trie.find(key_a), value_a);
  EXPECT_EQ(trie.find(HexArray("12")), nullptr);
  trie.Print();

  HexArray key_b("7ead1e640fb4854117ef3f882338ae");
  std::string value_b = "hello again";
  trie.insert(key_b, value_b);
  EXPECT_EQ(*trie.find(key_a), value_a);
  EXPECT_EQ(*trie.find(key_b), value_b);
  trie.Print();

  HexArray key_c("76");
  std::string value_c = "hello three";
  trie.insert(key_c, value_c);
  EXPECT_EQ(*trie.find(key_a), value_a);
  EXPECT_EQ(*trie.find(key_b), value_b);
  EXPECT_EQ(*trie.find(key_c), value_c);
  trie.Print();
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
  trie.insert(key_a, value_a);
  trie.for_each(f);
  std::map<HexArray, std::string> expected = {{key_a, value_a}};
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

TEST(MerkleTrie, TwoLeavesAndOneBranchRemove)
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

TEST(MerkleTrie, TwoLeavesAndOneBranchProve)
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

  EXPECT_TRUE(MerkleTrie::Prove(node_encodings, root_hash, key_b, value_b));
  EXPECT_FALSE(MerkleTrie::Prove(node_encodings, root_hash, key_a, value_a));
  EXPECT_FALSE(MerkleTrie::Prove(node_encodings, root_hash, key_b, value_c));
  EXPECT_FALSE(MerkleTrie::Prove(node_encodings, root_hash, key_c, value_b));

  EXPECT_FALSE(
      MerkleTrie::Prove(node_encodings, root_hash, HexArray(), value_b));
  EXPECT_FALSE(MerkleTrie::Prove(node_encodings, root_hash, key_b, ""));
  EXPECT_FALSE(MerkleTrie::Prove(node_encodings, root_hash,
                                 key_b.GetSubarray(1, key_b.size()), value_b));
  {
    HexArray key_b_copy = key_b;
    key_b_copy.Append(std::byte{0x00});
    EXPECT_FALSE(
        MerkleTrie::Prove(node_encodings, root_hash, key_b_copy, value_b));
  }

  EXPECT_FALSE(MerkleTrie::Prove({}, root_hash, key_b, value_b));
  std::vector<std::byte> copy(node_encodings.begin() + 1, node_encodings.end());
  EXPECT_FALSE(MerkleTrie::Prove(copy, root_hash, key_b, value_b));
  copy = std::vector(node_encodings.begin(), node_encodings.end() - 1);
  EXPECT_FALSE(MerkleTrie::Prove(copy, root_hash, key_b, value_b));

  // Try an incorrect root hash.
  {
    SHA256Hash different_root_hash;
    memset(&different_root_hash, 0, sizeof(different_root_hash));
    EXPECT_FALSE(MerkleTrie::Prove(copy, different_root_hash, key_b, value_b));
  }

  // Try using the hash of an internal node as the root hash.
  {
    SHA256Hash different_root_hash = sha256(node_encodings_by_node[1]);
    EXPECT_FALSE(MerkleTrie::Prove(copy, different_root_hash, key_b, value_b));
  }

  // Remove the leaf.
  copy = std::vector(node_encodings.begin() + node_encodings_by_node[0].size(),
                     node_encodings.end());
  EXPECT_FALSE(MerkleTrie::Prove(copy, root_hash, key_b, value_b));

  // Remove the root.
  copy =
      std::vector(node_encodings.begin(),
                  node_encodings.end() - node_encodings_by_node.back().size());
  EXPECT_FALSE(MerkleTrie::Prove(copy, root_hash, key_b, value_b));

  // Remove an intermediate node.
  copy = std::vector(node_encodings.begin(),
                     node_encodings.begin() + node_encodings_by_node[0].size());
  copy.insert(copy.end(),
              node_encodings.begin() + node_encodings_by_node[0].size() +
                  node_encodings_by_node[1].size(),
              node_encodings.end());
  EXPECT_FALSE(MerkleTrie::Prove(copy, root_hash, key_b, value_b));

  // Remove a byte from an intermediate node and test that the improper encoding
  // is detected.
  // TODO: Do this for all node types.
  copy = node_encodings;
  copy.erase(copy.begin() + node_encodings_by_node[0].size());
  EXPECT_FALSE(MerkleTrie::Prove(copy, root_hash, key_b, value_b));

  // Change the path in the leaf node.
  copy = node_encodings;
  copy[1 + sizeof(size_t)] = std::byte{0x00};
  EXPECT_FALSE(MerkleTrie::Prove(copy, root_hash, key_b, value_b));

  // Change the path in an extension node.
  size_t idx = 0;
  size_t node_idx = 0;
  bool found_extension = false;
  for (node_idx = 0; node_idx < node_encodings_by_node.size(); node_idx++) {
    if (static_cast<MerkleTrie::NodeType>(node_encodings[idx]) ==
        MerkleTrie::NodeType::kExtension) {
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
  EXPECT_FALSE(MerkleTrie::Prove(copy, root_hash, key_b, value_b));

  // Change the child hash in an extension node.
  {
    size_t end_idx = node_encodings_by_node[node_idx].size();
    copy = node_encodings;
    uint8_t result = static_cast<uint8_t>(copy[end_idx - 1]) + 1;
    copy[end_idx - 1] = static_cast<std::byte>(result);
    EXPECT_FALSE(MerkleTrie::Prove(copy, root_hash, key_b, value_b));
  }

  // Change the value in a leaf.
  {
    size_t end_idx = node_encodings_by_node[0].size();
    copy = node_encodings;
    uint8_t result = static_cast<uint8_t>(copy[end_idx - 1]) + 1;
    copy[end_idx - 1] = static_cast<std::byte>(result);
    EXPECT_FALSE(MerkleTrie::Prove(copy, root_hash, key_b, value_b));
  }

  // Change the matching hash in a branch.
  idx = 0;
  bool found_branch = false;
  for (node_idx = 0; node_idx < node_encodings_by_node.size(); node_idx++) {
    if (static_cast<MerkleTrie::NodeType>(node_encodings[idx]) ==
        MerkleTrie::NodeType::kBranch) {
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
  size_t branch_start_idx =
      idx + node_encodings_by_node[node_idx].size() - (16 * sizeof(SHA256Hash));
  SHA256Hash to_find = sha256(node_encodings_by_node[node_idx - 1]);
  for (size_t i = 0; i < 16; i++) {
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
    EXPECT_FALSE(MerkleTrie::Prove(copy, root_hash, key_b, value_b));
  }

  // Change a non-matching hash in a branch. We will just choose a different
  // branch from above.
  int nonmatching_idx = matching_idx + sizeof(SHA256Hash);
  nonmatching_idx %= 16 * sizeof(SHA256Hash);
  copy = node_encodings;

  {
    std::byte &change =
        copy[branch_start_idx + (nonmatching_idx * sizeof(SHA256Hash))];
    uint8_t result = static_cast<uint8_t>(change) + 1;
    change = static_cast<std::byte>(result);
    EXPECT_FALSE(MerkleTrie::Prove(copy, root_hash, key_b, value_b));
  }

  // Change the ordering of the nodes in a branch.
  copy = node_encodings;
  SHA256Hash &matching_hash =
      *reinterpret_cast<SHA256Hash *>(&copy[matching_idx]);
  SHA256Hash &nonmatching_hash =
      *reinterpret_cast<SHA256Hash *>(&copy[nonmatching_idx]);
  std::swap(matching_hash, nonmatching_hash);
  EXPECT_FALSE(MerkleTrie::Prove(copy, root_hash, key_b, value_b));
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
      "c396154ee62afd8f49537a36005f8e97d0fe82d9aadc78d7a6874e1cf762e705");
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
      "8b50bf2634e8ea11cefb85508e68626687801649d60a33e607ecd504cfa92f1c");
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
      "3fdeffbb6a2bc7f94a7f852f802fcc3d93635a0b39ed4a830dbef6598cf48787");
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
      "d2af4c9af613a07327407c683265444011144af1e1b1658a9ff4bf14acbe930a");
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
      "ed601bfeacd678e5814cb9adf207256004ce70dc2e41b8eda66ea002a5a03904");
  EXPECT_EQ(hash, expected);
}

namespace {

HexArray
GenerateRandomPath()
{
  std::random_device rd;
  std::mt19937 gen(rd());

  std::uniform_int_distribution<size_t> path_size_dist(1, 20);
  size_t path_size = path_size_dist(gen);

  std::vector<std::byte> path(path_size);
  std::uniform_int_distribution<int> path_dist(0, 0xff);

  for (std::byte &b : path) {
    b = static_cast<std::byte>(path_dist(gen));
  }
  // printf("path: %s (size %zu)\n", HexArray(path).str().c_str(), path.size());
  return HexArray(path);
}

} // namespace

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
    printf("i = %zu (insert key %s)\n", i, path.str().c_str());
    const std::string value = std::to_string(i);
    // printf("Insert...\n");
    bool insert = trie.insert(path, value);
    EXPECT_EQ(insert, total.count(path) == 0);
    if (!insert) {
      // A duplicate key was generated.
      continue;
    }

    // printf("Done inserting.\n");
    total.insert({path, value});
    // trie.Print();

    seen.clear();
    // printf("For each...\n");
    trie.for_each(f);
    // printf("Done with for each.\n");
    if (seen != total) {
      printf("seen (size %zu):\n", seen.size());
      for (const auto &[k, v] : seen) {
        printf("%s: %s\n", k.str().c_str(), v.c_str());
      }
      printf("\n");
      printf("total (size %zu):\n", total.size());
      for (const auto &[k, v] : total) {
        printf("%s: %s\n", k.str().c_str(), v.c_str());
      }
      printf("\n");

      HexArray diff;
      for (const auto &[k, v] : total) {
        if (seen.count(k) == 0) {
          diff = k;
          break;
        }
        if (seen[k] != v) {
          diff = k;
          break;
        }
      }
      printf("Diff key: %s\n", diff.str().c_str());
    }
    EXPECT_EQ(seen, total);

    // printf("Get encoding A.\n");
    const auto &[root_hash, node_encodings] = trie.GetProof(path);
    // printf("Now get encoding B.\n");
    const auto &[_, node_by_node] = trie.GetProofByNode(path);
    /*
    printf("to repeat, we are inserting %s with value %s\n", path.str().c_str(),
           value.c_str());
    printf("last node encoding: ");
    for (std::byte b : node_by_node.back()) {
      printf("%02x", b);
    }
    */
    // printf("\n");
    // printf("Prove...\n");
    EXPECT_TRUE(MerkleTrie::Prove(node_encodings, root_hash, path, value));
    // printf("Proved. Now find...\n");
    EXPECT_EQ(*trie.find(path), value);
    // printf("Found.\n");
  }

  for (const auto &[path, value] : total) {
    const auto &[root_hash, node_encodings] = trie.GetProof(path);
    EXPECT_TRUE(MerkleTrie::Prove(node_encodings, root_hash, path, value));

    EXPECT_EQ(*trie.find(path), value);
  }

  trie.Print();

  for (size_t i = 0; i < kNumIterations; i++) {
    const auto &[path, value] = *total.begin();
    const auto &[root_hash, node_encodings] = trie.GetProof(path);
    EXPECT_TRUE(MerkleTrie::Prove(node_encodings, root_hash, path, value));

    // printf("remove %s\n", path.str().c_str());
    EXPECT_EQ(trie.remove(path), 1);
    // trie.Print();
    auto it = total.erase(total.begin());
    if (i < kNumIterations - 1) {
      EXPECT_NE(it, total.end());
    }
    else {
      EXPECT_EQ(it, total.end());
    }

    EXPECT_EQ(trie.find(path), nullptr);

    seen.clear();
    trie.for_each(f);
    EXPECT_EQ(seen, total);
  }

  SHA256Hash root_hash = trie.GetRootHash();
  trie.RegenerateAllHashes(trie.root_);
  EXPECT_EQ(trie.GetRootHash(), root_hash);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
