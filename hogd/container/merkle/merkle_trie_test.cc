#include <cstddef>
#include <cstdio>
#include <cstring>
#include <future>
#include <gtest/gtest.h>
#include <iostream>

#include <map>
#include <memory>
#include <print>
#include <random>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/container/merkle/node.h"
#include "hogd/crypto/crypto.h"
#include "types.hh"

namespace hogd {

namespace {

// TODO(jhumphri): Compare with an existing Merkle Trie implementation to ensure
// correctness.

// Generates a random path with anywhere from 0 to 40 hex digits (inclusive).
HexArray
generate_random_path(bool allow_empty)
{
  std::random_device rd;
  std::mt19937 gen(rd());

  std::uniform_int_distribution<size_t> path_size_dist(allow_empty ? 0 : 1,
                                                       128);
  size_t path_size = path_size_dist(gen);

  // [path_size] may be odd, but [path] can only hold an even number of hex
  // digits because there are two hex digits in an [std::byte]. We remove the
  // extra hex digit below before returning the path.
  std::vector<std::byte> path((path_size + 1) / 2);
  std::uniform_int_distribution<int> path_dist(0, 0xff);

  for (auto &i : path) {
    i = static_cast<std::byte>(path_dist(gen));
  }
  HexArray ret(path);
  // If [path_size] is odd, remove a hex digit from the from path.
  if (path_size % 2) {
    ret.remove_from_back(1);
  }
  return ret;
}

void
for_each_func(std::map<HexArray, SHA256Hash> &seen,
              const std::pair<HexArray, SHA256Hash> &pair)
{
  seen[pair.first] = pair.second;
}

} // namespace

// Tests that [size()] and [empty()] return expected values.
TEST(MerkleTrie, SizeAndEmpty)
{
  MerkleTrie trie;

  EXPECT_EQ(trie.size(), 0);
  EXPECT_TRUE(trie.empty());

  const HexArray key("12");
  std::string value = "hello";
  std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                  value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));
  EXPECT_EQ(trie.size(), 1);
  EXPECT_FALSE(trie.empty());

  EXPECT_TRUE(trie.remove(key));
  EXPECT_EQ(trie.size(), 0);
  EXPECT_TRUE(trie.empty());
}

// These tests add an empty key to the trie.
TEST(MerkleTrie, EmptyKey)
{
  MerkleTrie trie;
  std::cout << trie << '\n';

  HexArray key;
  std::string value = "hello";
  std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                  value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));
  std::cout << trie << '\n';
}

TEST(MerkleTrie, EmptyKeyFind)
{
  MerkleTrie trie;

  HexArray key;
  std::string value = "hello";
  std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                  value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));
  SHA256Hash *value_hash;
  void *cookie;
  EXPECT_TRUE(trie.find_begin(key, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value));
  hogd::MerkleTrie::find_end(cookie);
}

TEST(MerkleTrie, EmptyKeyRemove)
{
  MerkleTrie trie;

  HexArray key;
  std::string value = "hello";
  std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                  value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));
  std::cout << trie << '\n';
  EXPECT_TRUE(trie.remove(key));
  std::cout << trie << '\n';
}

TEST(MerkleTrie, EmptyKeyRemoveWithChild)
{
  MerkleTrie trie;

  HexArray key_a;
  std::string value_a = "hello";
  std::span<std::byte> value_a_span{reinterpret_cast<std::byte *>(
                                        value_a.data()),
                                    value_a.size()};
  EXPECT_TRUE(trie.insert(key_a, value_a_span));

  HexArray key_b = HexArray("1");
  std::string value_b = "hello again";
  std::span<std::byte> value_b_span{reinterpret_cast<std::byte *>(
                                        value_b.data()),
                                    value_b.size()};
  EXPECT_TRUE(trie.insert(key_b, value_b_span));

  std::cout << trie << '\n';
  EXPECT_TRUE(trie.remove(key_a));
  std::cout << trie << '\n';
}

TEST(MerkleTrie, EmptyKeyRemoveChild)
{
  MerkleTrie trie;

  HexArray key_a;
  std::string value_a = "hello";
  std::span<std::byte> value_a_span{reinterpret_cast<std::byte *>(
                                        value_a.data()),
                                    value_a.size()};
  EXPECT_TRUE(trie.insert(key_a, value_a_span));

  HexArray key_b = HexArray("1");
  std::string value_b = "hello again";
  std::span<std::byte> value_b_span{reinterpret_cast<std::byte *>(
                                        value_b.data()),
                                    value_b.size()};
  EXPECT_TRUE(trie.insert(key_b, value_b_span));

  std::cout << trie << '\n';
  EXPECT_TRUE(trie.remove(key_b));
  std::cout << trie << '\n';
}

// Tests that [MerkleTriePrefix] does not work with empty keys.
TEST(MerkleTriePrefix, EmptyKey)
{
  MerkleTriePrefix trie;

  HexArray key;
  std::string value = "hello";
  std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                  value.size()};
  EXPECT_FALSE(trie.insert(key, value_span));
  SHA256Hash *value_hash;
  void *cookie;
  EXPECT_FALSE(trie.find_begin(key, value_hash, cookie));
  EXPECT_FALSE(trie.remove(key));
  EXPECT_TRUE(trie.get_proof(key).second.empty());
  EXPECT_TRUE(trie.get_proof_by_node(key).second.empty());
}

// These tests add an empty value to the trie.
TEST(MerkleTrie, EmptyValue)
{
  MerkleTrie trie;
  std::cout << trie << '\n';

  HexArray key("1");
  EXPECT_TRUE(trie.insert(key, std::span<std::byte>()));
  std::cout << trie << '\n';
}

TEST(MerkleTrie, EmptyValueFind)
{
  MerkleTrie trie;

  HexArray key("1");
  std::string value;
  std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                  value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));
  SHA256Hash *value_hash;
  void *cookie;
  EXPECT_TRUE(trie.find_begin(key, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value));
  hogd::MerkleTrie::find_end(cookie);
}

TEST(MerkleTrie, EmptyValueRemove)
{
  MerkleTrie trie;

  HexArray key("1");
  std::string value;
  std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                  value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));
  std::cout << trie << '\n';
  EXPECT_TRUE(trie.remove(key));
  std::cout << trie << '\n';
}

// These tests add keys of even and/or odd size to the trie.
TEST(MerkleTrie, EvenSizeKey)
{
  MerkleTrie trie;

  HexArray key("01");
  std::string value = "hello";
  std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                  value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));
  SHA256Hash *value_hash;
  void *cookie;
  EXPECT_TRUE(trie.find_begin(key, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value));
  hogd::MerkleTrie::find_end(cookie);
}

TEST(MerkleTrie, OddSizeKey)
{
  MerkleTrie trie;

  HexArray key("012");
  std::string value = "hello";
  std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                  value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));
  SHA256Hash *value_hash;
  void *cookie;
  EXPECT_TRUE(trie.find_begin(key, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value));
  hogd::MerkleTrie::find_end(cookie);
}

TEST(MerkleTrie, EvenAndOddSizedKeys)
{
  MerkleTrie trie;

  HexArray even_key("01");
  std::string even_value = "hello";
  std::span<std::byte> even_value_span{reinterpret_cast<std::byte *>(
                                           even_value.data()),
                                       even_value.size()};
  EXPECT_TRUE(trie.insert(even_key, even_value_span));

  HexArray odd_key("012");
  std::string odd_value = "hello";
  std::span<std::byte> odd_value_span{reinterpret_cast<std::byte *>(
                                          odd_value.data()),
                                      odd_value.size()};
  EXPECT_TRUE(trie.insert(odd_key, odd_value_span));

  SHA256Hash *value_hash;
  void *cookie;
  EXPECT_TRUE(trie.find_begin(even_key, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(even_value));
  hogd::MerkleTrie::find_end(cookie);

  EXPECT_TRUE(trie.find_begin(odd_key, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(odd_value));
  hogd::MerkleTrie::find_end(cookie);
}

// This tests that [insert()] fails when attempting to insert a duplicate key.
TEST(MerkleTrie, InsertDuplicateKey)
{
  MerkleTrie trie;

  HexArray key("1234");
  std::string value = "hello";
  std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                  value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));

  // Attempt to insert the same key again.
  std::string other_value = "hello again";
  std::span<std::byte> other_value_span{reinterpret_cast<std::byte *>(
                                            other_value.data()),
                                        other_value.size()};
  EXPECT_FALSE(trie.insert(key, other_value_span));

  SHA256Hash *value_hash;
  void *cookie;
  EXPECT_TRUE(trie.find_begin(key, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value));
  hogd::MerkleTrie::find_end(cookie);
}

// This tests that [remove()] fails when attempting to remove a key that is not
// in the trie.
TEST(MerkleTrie, RemoveMissingKey)
{
  MerkleTrie trie;

  HexArray key("1234");
  std::string value = "hello";
  std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                  value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));
  EXPECT_TRUE(trie.remove(key));

  // Attempt to remove the same key again.
  EXPECT_FALSE(trie.remove(key));
}

// These tests add two KV pairs to the trie, with both KV pairs being in leaf
// nodes.
TEST(MerkleTrie, TwoLeaves)
{
  MerkleTrie trie;
  std::cout << trie << '\n';

  HexArray key("1234");
  std::string value = "hello";
  std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                  value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));
  std::cout << trie << '\n';

  key = HexArray("1223");
  value = "hello again";
  value_span = std::span<std::byte>{reinterpret_cast<std::byte *>(value.data()),
                                    value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));
  std::cout << trie << '\n';
}

TEST(MerkleTrie, TwoLeavesFind)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  std::span<std::byte> value_a_span{reinterpret_cast<std::byte *>(
                                        value_a.data()),
                                    value_a.size()};
  EXPECT_TRUE(trie.insert(key_a, value_a_span));
  SHA256Hash *value_hash;
  void *cookie;
  EXPECT_TRUE(trie.find_begin(key_a, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_a));
  hogd::MerkleTrie::find_end(cookie);
  EXPECT_FALSE(trie.find_begin(HexArray("12"), value_hash, cookie));

  HexArray key_b("1223");
  std::string value_b = "hello again";
  std::span<std::byte> value_b_span{reinterpret_cast<std::byte *>(
                                        value_b.data()),
                                    value_b.size()};
  EXPECT_TRUE(trie.insert(key_b, value_b_span));
  EXPECT_TRUE(trie.find_begin(key_a, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_a));
  hogd::MerkleTrie::find_end(cookie);
  EXPECT_TRUE(trie.find_begin(key_b, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_b));
  hogd::MerkleTrie::find_end(cookie);
  EXPECT_FALSE(trie.find_begin(HexArray("12"), value_hash, cookie));
  EXPECT_FALSE(trie.find_begin(HexArray("123456"), value_hash, cookie));
}

TEST(MerkleTrie, TwoLeavesRemove)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  std::span<std::byte> value_a_span{reinterpret_cast<std::byte *>(
                                        value_a.data()),
                                    value_a.size()};
  EXPECT_TRUE(trie.insert(key_a, value_a_span));

  HexArray key_b("1223");
  std::string value_b = "hello again";
  std::span<std::byte> value_b_span{reinterpret_cast<std::byte *>(
                                        value_b.data()),
                                    value_b.size()};
  EXPECT_TRUE(trie.insert(key_b, value_b_span));
  std::cout << trie << '\n';
  EXPECT_TRUE(trie.remove(key_b));
  std::cout << trie << '\n';
}

// These tests add two KV pairs to the trie, with one KV pair being in a branch
// node and the other being in a leaf node below the branch.
TEST(MerkleTrie, BranchAndLeaf)
{
  MerkleTrie trie;
  std::cout << trie << '\n';

  HexArray key("1234");
  std::string value = "hello";
  std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                  value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));
  std::cout << trie << '\n';

  key = HexArray("123456");
  value = "hello again";
  value_span = std::span<std::byte>{reinterpret_cast<std::byte *>(value.data()),
                                    value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));
  std::cout << trie << '\n';
}

TEST(MerkleTrie, BranchAndLeafRemove)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  std::span<std::byte> value_a_span{reinterpret_cast<std::byte *>(
                                        value_a.data()),
                                    value_a.size()};
  EXPECT_TRUE(trie.insert(key_a, value_a_span));

  HexArray key_b = HexArray("123456");
  std::string value_b = "hello again";
  std::span<std::byte> value_b_span{reinterpret_cast<std::byte *>(
                                        value_b.data()),
                                    value_b.size()};
  EXPECT_TRUE(trie.insert(key_b, value_b_span));

  std::cout << trie << '\n';
  EXPECT_TRUE(trie.remove(key_b));
  std::cout << trie << '\n';
}

TEST(MerkleTrie, BranchAndLeafFind)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  std::span<std::byte> value_a_span{reinterpret_cast<std::byte *>(
                                        value_a.data()),
                                    value_a.size()};
  EXPECT_TRUE(trie.insert(key_a, value_a_span));
  SHA256Hash *value_hash;
  void *cookie;
  EXPECT_TRUE(trie.find_begin(key_a, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_a));
  hogd::MerkleTrie::find_end(cookie);
  EXPECT_FALSE(trie.find_begin(HexArray("12"), value_hash, cookie));

  HexArray key_b("123456");
  std::string value_b = "hello again";
  std::span<std::byte> value_b_span{reinterpret_cast<std::byte *>(
                                        value_b.data()),
                                    value_b.size()};
  EXPECT_TRUE(trie.insert(key_b, value_b_span));
  std::cout << trie << '\n';
  EXPECT_TRUE(trie.find_begin(key_a, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_a));
  hogd::MerkleTrie::find_end(cookie);
  EXPECT_TRUE(trie.find_begin(key_b, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_b));
  hogd::MerkleTrie::find_end(cookie);
  EXPECT_FALSE(trie.find_begin(HexArray("12"), value_hash, cookie));
  EXPECT_FALSE(trie.find_begin(HexArray("12345678"), value_hash, cookie));
}

// This tests that a branch node with an empty value (i.e., a value of
// [std::nullopt]) is not mistakenly thought to contain a KV pair.
TEST(MerkleTrie, DoNotfindBranchValue)
{
  MerkleTrie trie;

  HexArray key_a("123456");
  std::string value_a = "hello";
  std::span<std::byte> value_a_span{reinterpret_cast<std::byte *>(
                                        value_a.data()),
                                    value_a.size()};
  EXPECT_TRUE(trie.insert(key_a, value_a_span));

  HexArray key_b("123478");
  std::string value_b = "hello again";
  std::span<std::byte> value_b_span{reinterpret_cast<std::byte *>(
                                        value_b.data()),
                                    value_b.size()};
  EXPECT_TRUE(trie.insert(key_b, value_b_span));

  // "1234" maps to a branch node that has no value (i.e., [node->value] is
  // [std::nullopt]). Check that we can properly detect this and return
  // [nullptr] rather than mistakenly return an empty string or something
  // similar.
  SHA256Hash *value_hash;
  void *cookie;
  EXPECT_FALSE(trie.find_begin(HexArray("1234"), value_hash, cookie));
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
  std::span<std::byte> value_a_span{reinterpret_cast<std::byte *>(
                                        value_a.data()),
                                    value_a.size()};
  EXPECT_TRUE(trie.insert(key_a, value_a_span));
  SHA256Hash *value_hash;
  void *cookie;
  EXPECT_TRUE(trie.find_begin(key_a, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_a));
  hogd::MerkleTrie::find_end(cookie);
  EXPECT_FALSE(trie.find_begin(HexArray("12"), value_hash, cookie));
  std::cout << trie << '\n';

  HexArray key_b("7ead1e640fb4854117ef3f882338ae");
  std::string value_b = "hello again";
  std::span<std::byte> value_b_span{reinterpret_cast<std::byte *>(
                                        value_b.data()),
                                    value_b.size()};
  EXPECT_TRUE(trie.insert(key_b, value_b_span));
  EXPECT_TRUE(trie.find_begin(key_a, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_a));
  hogd::MerkleTrie::find_end(cookie);
  EXPECT_TRUE(trie.find_begin(key_b, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_b));
  hogd::MerkleTrie::find_end(cookie);
  std::cout << trie << '\n';

  HexArray key_c("76");
  std::string value_c = "hello three";
  std::span<std::byte> value_c_span{reinterpret_cast<std::byte *>(
                                        value_c.data()),
                                    value_c.size()};
  EXPECT_TRUE(trie.insert(key_c, value_c_span));
  EXPECT_TRUE(trie.find_begin(key_a, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_a));
  hogd::MerkleTrie::find_end(cookie);
  EXPECT_TRUE(trie.find_begin(key_b, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_b));
  hogd::MerkleTrie::find_end(cookie);
  EXPECT_TRUE(trie.find_begin(key_c, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_c));
  hogd::MerkleTrie::find_end(cookie);
  std::cout << trie << '\n';

  trie.generate_top_level_hashes();
  // We manually observed this root hash previously.
  EXPECT_EQ(
      HexArray(trie.get_root_hash()),
      HexArray(
          "b5f9b456037a1449926a578ec7bcf9b1a0f0815e059185ac42d55bebb757ad1d"));
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
  std::cout << trie << '\n';

  HexArray key("1234");
  std::string value = "hello";
  std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                  value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));
  std::cout << trie << '\n';

  key = HexArray("123456");
  value = "hello again";
  value_span = std::span<std::byte>{reinterpret_cast<std::byte *>(value.data()),
                                    value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));
  std::cout << trie << '\n';

  key = HexArray("12ab");
  value = "hello three";
  value_span = std::span<std::byte>{reinterpret_cast<std::byte *>(value.data()),
                                    value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));
  std::cout << trie << '\n';
}

TEST(MerkleTrie, TwoLeavesAndOneBranchFind)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  std::span<std::byte> value_a_span{reinterpret_cast<std::byte *>(
                                        value_a.data()),
                                    value_a.size()};
  EXPECT_TRUE(trie.insert(key_a, value_a_span));
  SHA256Hash *value_hash;
  void *cookie;
  EXPECT_TRUE(trie.find_begin(key_a, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_a));
  hogd::MerkleTrie::find_end(cookie);
  EXPECT_FALSE(trie.find_begin(HexArray("12"), value_hash, cookie));

  HexArray key_b("123456");
  std::string value_b = "hello again";
  std::span<std::byte> value_b_span{reinterpret_cast<std::byte *>(
                                        value_b.data()),
                                    value_b.size()};
  EXPECT_TRUE(trie.insert(key_b, value_b_span));
  EXPECT_TRUE(trie.find_begin(key_a, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_a));
  hogd::MerkleTrie::find_end(cookie);
  EXPECT_TRUE(trie.find_begin(key_b, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_b));
  hogd::MerkleTrie::find_end(cookie);
  EXPECT_FALSE(trie.find_begin(HexArray("12"), value_hash, cookie));
  EXPECT_FALSE(trie.find_begin(HexArray("12345678"), value_hash, cookie));

  HexArray key_c("12ab");
  std::string value_c = "hello three";
  std::span<std::byte> value_c_span{reinterpret_cast<std::byte *>(
                                        value_c.data()),
                                    value_c.size()};
  EXPECT_TRUE(trie.insert(key_c, value_c_span));
  EXPECT_TRUE(trie.find_begin(key_a, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_a));
  hogd::MerkleTrie::find_end(cookie);
  EXPECT_TRUE(trie.find_begin(key_b, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_b));
  hogd::MerkleTrie::find_end(cookie);
  EXPECT_TRUE(trie.find_begin(key_c, value_hash, cookie));
  EXPECT_EQ(*value_hash, sha256(value_c));
  hogd::MerkleTrie::find_end(cookie);
  EXPECT_FALSE(trie.find_begin(HexArray("12"), value_hash, cookie));
  EXPECT_FALSE(trie.find_begin(HexArray("12abcd"), value_hash, cookie));
  EXPECT_FALSE(trie.find_begin(HexArray("12345678"), value_hash, cookie));
}

TEST(MerkleTrie, TwoLeavesAndOneBranchForEach)
{
  MerkleTrie trie;
  std::map<HexArray, SHA256Hash> seen;
  auto f = [&seen](const std::pair<HexArray, SHA256Hash> &pair) {
    for_each_func(seen, pair);
  };

  HexArray key_a("1234");
  std::string value_a = "hello";
  std::span<std::byte> value_a_span{reinterpret_cast<std::byte *>(
                                        value_a.data()),
                                    value_a.size()};
  EXPECT_TRUE(trie.insert(key_a, value_a_span));
  trie.for_each(f);
  std::map<HexArray, SHA256Hash> expected = {{key_a, sha256(value_a)}};
  EXPECT_EQ(seen, expected);

  HexArray key_b("123456");
  std::string value_b = "hello again";
  std::span<std::byte> value_b_span{reinterpret_cast<std::byte *>(
                                        value_b.data()),
                                    value_b.size()};
  EXPECT_TRUE(trie.insert(key_b, value_b_span));
  seen.clear();
  expected.insert({{key_b, sha256(value_b)}});
  trie.for_each(f);
  EXPECT_EQ(seen, expected);

  HexArray key_c("12ab");
  std::string value_c = "hello three";
  std::span<std::byte> value_c_span{reinterpret_cast<std::byte *>(
                                        value_c.data()),
                                    value_c.size()};
  EXPECT_TRUE(trie.insert(key_c, value_c_span));
  seen.clear();
  expected.insert({{key_c, sha256(value_c)}});
  trie.for_each(f);
  EXPECT_EQ(seen, expected);
}

TEST(MerkleTrie, TwoLeavesAndOneBranchRemove)
{
  MerkleTrie trie;

  HexArray key_a("1234");
  std::string value_a = "hello";
  std::span<std::byte> value_a_span{reinterpret_cast<std::byte *>(
                                        value_a.data()),
                                    value_a.size()};
  EXPECT_TRUE(trie.insert(key_a, value_a_span));

  HexArray key_b("123456");
  std::string value_b = "hello again";
  std::span<std::byte> value_b_span{reinterpret_cast<std::byte *>(
                                        value_b.data()),
                                    value_b.size()};
  EXPECT_TRUE(trie.insert(key_b, value_b_span));

  HexArray key_c("12ab");
  std::string value_c = "hello three";
  std::span<std::byte> value_c_span{reinterpret_cast<std::byte *>(
                                        value_c.data()),
                                    value_c.size()};
  EXPECT_TRUE(trie.insert(key_c, value_c_span));

  std::cout << trie << '\n';
  EXPECT_TRUE(trie.remove(key_b));
  std::cout << trie << '\n';
}

namespace {

// TODO(jhumphri): Clean this up further.
// This tests generating a proof and verifying a proof.
template<typename T>
void
prove(T &trie)
{
  HexArray key_a("1234");
  std::string value_a = "hello";
  std::span<std::byte> value_a_span{reinterpret_cast<std::byte *>(
                                        value_a.data()),
                                    value_a.size()};
  EXPECT_TRUE(trie.insert(key_a, value_a_span));

  HexArray key_b("123456");
  std::string value_b = "hello again";
  std::span<std::byte> value_b_span{reinterpret_cast<std::byte *>(
                                        value_b.data()),
                                    value_b.size()};
  EXPECT_TRUE(trie.insert(key_b, value_b_span));

  HexArray key_c("12ab");
  std::string value_c = "hello three";
  std::span<std::byte> value_c_span{reinterpret_cast<std::byte *>(
                                        value_c.data()),
                                    value_c.size()};
  EXPECT_TRUE(trie.insert(key_c, value_c_span));

  trie.generate_top_level_hashes();
  const auto &[root_hash, node_encodings] = trie.get_proof(key_b);
  const auto &[root_hash_by_node, node_encodings_by_node] =
      trie.get_proof_by_node(key_b);
  EXPECT_EQ(root_hash, root_hash_by_node);

  // [get_proof()] should be equivalent to [get_proof_by_node()] when the latter
  // is flattened from a 2D vector into a 1D vector.
  std::vector<std::byte> node_encodings_by_node_flat;
  for (const std::vector<std::byte> &node_encoding : node_encodings_by_node) {
    node_encodings_by_node_flat.insert(node_encodings_by_node_flat.end(),
                                       node_encoding.begin(),
                                       node_encoding.end());
  }
  EXPECT_EQ(node_encodings, node_encodings_by_node_flat);

  // [key_b/value_b] is in the trie, so we should be able to prove it.
  EXPECT_TRUE(
      MerkleTrie::prove(root_hash, node_encodings, key_b, value_b_span));

  // None of these pairs are in the trie, so we should not be able to prove
  // them.
  EXPECT_FALSE(
      MerkleTrie::prove(root_hash, node_encodings, key_a, value_a_span));
  EXPECT_FALSE(
      MerkleTrie::prove(root_hash, node_encodings, key_b, value_c_span));
  EXPECT_FALSE(
      MerkleTrie::prove(root_hash, node_encodings, key_c, value_b_span));

  // Try an empty key and an empty value.
  EXPECT_FALSE(
      MerkleTrie::prove(root_hash, node_encodings, HexArray(), value_b_span));
  EXPECT_FALSE(MerkleTrie::prove(root_hash, node_encodings, key_b,
                                 std::span<std::byte>()));

  // Try removing a hex digit from the key.
  EXPECT_FALSE(MerkleTrie::prove(root_hash, node_encodings,
                                 key_b.get_subarray(1, key_b.size()),
                                 value_b_span));

  {
    // Try adding a hex digit to the key.
    HexArray key_b_copy = key_b;
    key_b_copy.append(std::byte{0x00});
    EXPECT_FALSE(
        MerkleTrie::prove(root_hash, node_encodings, key_b_copy, value_b_span));
  }

  // Omit the node encodings.
  EXPECT_FALSE(MerkleTrie::prove(root_hash, {}, key_b, value_b_span));
  {
    // Remove a byte from the beginning of the node encodings.
    std::vector<std::byte> copy(node_encodings.begin() + 1,
                                node_encodings.end());
    EXPECT_FALSE(MerkleTrie::prove(root_hash, copy, key_b, value_b_span));
  }
  {
    // Remove a byte from the end of the node encodings.
    std::vector<std::byte> copy(node_encodings.begin(),
                                node_encodings.end() - 1);
    EXPECT_FALSE(MerkleTrie::prove(root_hash, copy, key_b, value_b_span));
  }

  {
    // Try an incorrect root hash.
    SHA256Hash different_root_hash;
    memset(&different_root_hash, 0, sizeof(different_root_hash));
    EXPECT_FALSE(MerkleTrie::prove(different_root_hash, node_encodings, key_b,
                                   value_b_span));
  }

  {
    // Try using the hash of an internal node as the root hash.
    SHA256Hash different_root_hash = sha256(node_encodings_by_node[1]);
    EXPECT_FALSE(MerkleTrie::prove(different_root_hash, node_encodings, key_b,
                                   value_b_span));
  }

  {
    // Remove the leaf.
    std::vector<std::byte> copy(node_encodings.begin() +
                                    node_encodings_by_node[0].size(),
                                node_encodings.end());
    EXPECT_FALSE(MerkleTrie::prove(root_hash, copy, key_b, value_b_span));
  }

  {
    // Remove the root.
    std::vector<std::byte> copy(node_encodings.begin(),
                                node_encodings.end() -
                                    node_encodings_by_node.back().size());
    EXPECT_FALSE(MerkleTrie::prove(root_hash, copy, key_b, value_b_span));
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
    EXPECT_FALSE(MerkleTrie::prove(root_hash, copy, key_b, value_b_span));
  }

  {
    // Set the node type to an invalid type.
    std::vector<std::byte> copy = node_encodings;
    copy[0] = std::byte{0xFF};
    EXPECT_FALSE(MerkleTrie::prove(root_hash, copy, key_b, value_b_span));
  }

  {
    // Remove a byte from an intermediate node and test that the improper
    // encoding is detected.
    std::vector<std::byte> copy = node_encodings;
    copy.erase(copy.begin() + node_encodings_by_node[0].size());
    EXPECT_FALSE(MerkleTrie::prove(root_hash, copy, key_b, value_b_span));
  }

  {
    // Change the path in the leaf node.
    std::vector<std::byte> copy = node_encodings;
    copy[1 + sizeof(size_t)] = std::byte{0x00};
    EXPECT_FALSE(MerkleTrie::prove(root_hash, copy, key_b, value_b_span));
  }

  {
    // Make the path longer than the [HexArray] capacity in a leaf node.
    std::vector<std::byte> copy = node_encodings;
    copy[1] = std::byte{0xFF};
    EXPECT_FALSE(MerkleTrie::prove(root_hash, copy, key_b, value_b_span));
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
      idx += node_encodings_by_node[node_idx].size();
    }
    ASSERT_TRUE(found_extension);
    {
      // Change the path in an extension node.
      std::vector<std::byte> copy = node_encodings;
      copy[idx + 1 + sizeof(size_t)] = std::byte{0x00};
      EXPECT_FALSE(MerkleTrie::prove(root_hash, copy, key_b, value_b_span));
    }

    {
      // Make the path longer than the [HexArray] capacity in an extension node.
      std::vector<std::byte> copy = node_encodings;
      copy[idx + 1] = std::byte{0xFF};
      EXPECT_FALSE(MerkleTrie::prove(root_hash, copy, key_b, value_b_span));
    }

    {
      // Change the child hash in an extension node.
      size_t end_idx = node_encodings_by_node[node_idx].size();
      std::vector<std::byte> copy = node_encodings;
      uint8_t result = static_cast<uint8_t>(copy[end_idx - 1]) + 1;
      copy[end_idx - 1] = static_cast<std::byte>(result);
      EXPECT_FALSE(MerkleTrie::prove(root_hash, copy, key_b, value_b_span));
    }
  }

  {
    // Change the value in a leaf.
    size_t end_idx = node_encodings_by_node[0].size();
    std::vector<std::byte> copy = node_encodings;
    uint8_t result = static_cast<uint8_t>(copy[end_idx - 1]) + 1;
    copy[end_idx - 1] = static_cast<std::byte>(result);
    EXPECT_FALSE(MerkleTrie::prove(root_hash, copy, key_b, value_b_span));
  }

  {
    int matching_idx = -1;
    int nonmatching_idx;
    size_t branch_begin_idx;
    {
      // Change the matching hash in a branch. [prove()] should verify that the
      // hash in a branch's child hash array matches the hash in the child node
      // itself. This test changes the hash in the branch's child hash array
      // which should cause [prove()] to return false.
      size_t idx = 0;
      bool found_branch = false;
      size_t node_idx;
      for (node_idx = 0; node_idx < node_encodings_by_node.size(); node_idx++) {
        if (static_cast<Node::Type>(node_encodings[idx]) ==
            Node::Type::kBranch) {
          found_branch = true;
          break;
        }
        idx += node_encodings_by_node[node_idx].size();
      }
      ASSERT_TRUE(found_branch);
      std::vector<std::byte> copy = node_encodings;
      // find matching branch.
      branch_begin_idx = idx + node_encodings_by_node[node_idx].size() -
                         (Node::kMaxNumBranches * sizeof(SHA256Hash));
      SHA256Hash to_find = sha256(node_encodings_by_node[node_idx - 1]);
      for (size_t i = 0; i < Node::kMaxNumBranches; i++) {
        auto *to_compare = reinterpret_cast<SHA256Hash *>(
            &copy[branch_begin_idx + (i * sizeof(SHA256Hash))]);
        if (*to_compare == to_find) {
          matching_idx = i;
          break;
        }
      }
      ASSERT_GE(matching_idx, 0);
      {
        std::byte &change =
            copy[branch_begin_idx + (matching_idx * sizeof(SHA256Hash))];
        uint8_t result = static_cast<uint8_t>(change) + 1;
        change = static_cast<std::byte>(result);
        EXPECT_FALSE(MerkleTrie::prove(root_hash, copy, key_b, value_b_span));
      }
    }

    {
      // Change a non-matching hash in a branch. We will just choose a different
      // branch from above. This will cause the hash we generate for the branch
      // node to change, which should cause [prove()] to return false.
      nonmatching_idx = matching_idx + sizeof(SHA256Hash);
      nonmatching_idx %= Node::kMaxNumBranches * sizeof(SHA256Hash);
      std::vector<std::byte> copy = node_encodings;

      {
        std::byte &change =
            copy[branch_begin_idx + (nonmatching_idx * sizeof(SHA256Hash))];
        uint8_t result = static_cast<uint8_t>(change) + 1;
        change = static_cast<std::byte>(result);
        EXPECT_FALSE(MerkleTrie::prove(root_hash, copy, key_b, value_b_span));
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
      EXPECT_FALSE(MerkleTrie::prove(root_hash, copy, key_b, value_b_span));
    }
  }
}

} // namespace

TEST(MerkleTrie, Prove)
{
  MerkleTrie trie;
  prove(trie);
}

TEST(MerkleTriePrefix, Prove)
{
  MerkleTriePrefix trie;
  prove(trie);
}

namespace {

// Test that we cannot prove a KV pair with an empty key and value is in an
// empty trie.
template<typename T>
void
prove_empty_trie(T &trie)
{
  HexArray key;
  const auto &[root_hash, node_encodings] = trie.get_proof(key);
  EXPECT_FALSE(MerkleTrie::prove(root_hash, node_encodings, key,
                                 std::span<std::byte>()));
}

} // namespace

TEST(MerkleTrie, ProveEmptyTrie)
{
  MerkleTrie trie;
  prove_empty_trie(trie);
}

TEST(MerkleTriePrefix, ProveEmptyTrie)
{
  MerkleTriePrefix trie;
  prove_empty_trie(trie);
}

// Test that we can prove a KV pair with an empty key and value is in the trie,
// and that we cannot prove it is in the trie once we remove it.
TEST(MerkleTrie, ProveEmptyKVPair)
{
  MerkleTrie trie;

  HexArray key;
  std::span<std::byte> value_span;
  EXPECT_TRUE(trie.insert(key, value_span));

  {
    const auto &[root_hash, node_encodings] = trie.get_proof(key);
    EXPECT_TRUE(MerkleTrie::prove(root_hash, node_encodings, key, value_span));
  }

  {
    EXPECT_TRUE(trie.remove(key));
    const auto &[root_hash, node_encodings] = trie.get_proof(key);
    EXPECT_FALSE(MerkleTrie::prove(root_hash, node_encodings, key, value_span));
  }
}

namespace {

// Test that we can no longer prove a KV pair is in the trie after removing it.
// Also check that after removign it, we cannot prove a KV pair with an empty
// key and value is in the trie.
template<typename T>
void
prove_after_removing_key(T &trie)
{
  HexArray key("1234");
  std::string value = "hello";
  std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                  value.size()};
  EXPECT_TRUE(trie.insert(key, value_span));
  EXPECT_TRUE(trie.remove(key));

  {
    const auto &[root_hash, node_encodings] = trie.get_proof(key);
    EXPECT_FALSE(MerkleTrie::prove(root_hash, node_encodings, key, value_span));
  }

  {
    key = HexArray();
    const auto &[root_hash, node_encodings] = trie.get_proof(key);
    EXPECT_FALSE(MerkleTrie::prove(root_hash, node_encodings, key,
                                   std::span<std::byte>()));
  }
}

} // namespace

TEST(MerkleTrie, ProveAfterRemovingKey)
{
  MerkleTrie trie;
  prove_after_removing_key(trie);
}

TEST(MerkleTriePrefix, ProveAfterRemovingKey)
{
  MerkleTriePrefix trie;
  prove_after_removing_key(trie);
}

// This tests that when we change each byte in the proof to every possible byte
// value (0x00 through 0xFF, inclusive), [prove()] returns false.
TEST(MerkleTrie, ProveAllByteVariations)
{
  MerkleTrie trie;

  HexArray key_a("12");
  std::string value_a = "a1";
  std::span<std::byte> value_a_span{reinterpret_cast<std::byte *>(
                                        value_a.data()),
                                    value_a.size()};
  EXPECT_TRUE(trie.insert(key_a, value_a_span));

  HexArray key_b("123");
  std::string value_b = "b2";
  std::span<std::byte> value_b_span{reinterpret_cast<std::byte *>(
                                        value_b.data()),
                                    value_b.size()};
  EXPECT_TRUE(trie.insert(key_b, value_b_span));

  auto [root_hash, node_encodings] = trie.get_proof(key_b);
  for (size_t i = 0; i < node_encodings.size(); i++) {
    // Change the byte at index [i] in [node_encodings].
    const std::byte cur = node_encodings[i];
    for (size_t j = 0; j <= 0xFF; j++) {
      auto to_set = std::byte(j);
      if (to_set == cur) {
        // Make sure we do not set the byte to its current value in
        // [node_encodings] as [MerkleTrie::prove()] will then return true
        // below.
        continue;
      }
      node_encodings[i] = to_set;
      EXPECT_FALSE(
          MerkleTrie::prove(root_hash, node_encodings, key_b, value_b_span));
    }
    node_encodings[i] = cur;
  }
  EXPECT_TRUE(
      MerkleTrie::prove(root_hash, node_encodings, key_b, value_b_span));
}

// [MerkleTriePrefix] includes an additional branch node in the proof than in
// [MerkleTrie]. Since we already test [MerkleTrie] above, we just change the
// bytes in the extra branch node in this test to reduce test runtime.
TEST(MerkleTriePrefix, ProveAllByteVariations)
{
  MerkleTriePrefix trie;

  HexArray key_a("12");
  std::string value_a = "a1";
  std::span<std::byte> value_a_span{reinterpret_cast<std::byte *>(
                                        value_a.data()),
                                    value_a.size()};
  EXPECT_TRUE(trie.insert(key_a, value_a_span));

  HexArray key_b("123");
  std::string value_b = "b2";
  std::span<std::byte> value_b_span{reinterpret_cast<std::byte *>(
                                        value_b.data()),
                                    value_b.size()};
  EXPECT_TRUE(trie.insert(key_b, value_b_span));

  auto [_, node_encodings_by_node] = trie.get_proof_by_node(key_b);
  auto [root_hash, node_encodings] = trie.get_proof(key_b);
  for (size_t i = 0; i < node_encodings_by_node[0].size(); i++) {
    size_t idx = node_encodings.size() - i - 1;
    const std::byte cur = node_encodings[idx];
    for (size_t j = 0; j <= 0xFF; j++) {
      auto to_set = std::byte(j);
      if (to_set == cur) {
        // Make sure we do not set the byte to its current value in
        // [node_encodings] as [MerkleTrie::prove()] will then return true
        // below.
        continue;
      }
      node_encodings[idx] = to_set;
      EXPECT_FALSE(
          MerkleTrie::prove(root_hash, node_encodings, key_b, value_b_span));
    }
    node_encodings[idx] = cur;
  }
  EXPECT_TRUE(
      MerkleTrie::prove(root_hash, node_encodings, key_b, value_b_span));
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
  std::span<std::byte> value_a_span{reinterpret_cast<std::byte *>(
                                        value_a.data()),
                                    value_a.size()};
  EXPECT_TRUE(trie.insert(key_a, value_a_span));

  HexArray key_b("267124564f6835");
  std::string value_b = "hello again";
  std::span<std::byte> value_b_span{reinterpret_cast<std::byte *>(
                                        value_b.data()),
                                    value_b.size()};
  EXPECT_TRUE(trie.insert(key_b, value_b_span));
  std::cout << trie << '\n';

  // This needs to be placed directly into the branch under the extension node.
  HexArray key_c("2671");
  std::string value_c = "hello three";
  std::span<std::byte> value_c_span{reinterpret_cast<std::byte *>(
                                        value_c.data()),
                                    value_c.size()};
  EXPECT_TRUE(trie.insert(key_c, value_c_span));
  std::cout << trie << '\n';

  std::map<HexArray, SHA256Hash> seen;
  auto f = [&seen](const std::pair<HexArray, SHA256Hash> &pair) {
    for_each_func(seen, pair);
  };
  trie.for_each(f);
  std::map<HexArray, SHA256Hash> expected = {{key_a, sha256(value_a)},
                                             {key_b, sha256(value_b)},
                                             {key_c, sha256(value_c)}};
  EXPECT_EQ(seen, expected);
}

namespace {

// This test stresses the Merkle trie. It randomly generates 100 different KV
// pairs, places them into the trie, and then removes them from the trie. After
// each operation, this test checks that [for_each()], [get_proof()], [prove()],
// and [find_begin()] return expected values. This test also performs an
// integrity check on the trie after each operation with [check()].
template<typename T>
void
stress(T &trie, bool allow_empty)
{
  constexpr size_t kNumIterations = 100;

  std::map<HexArray, std::string> total;
  std::map<HexArray, SHA256Hash> seen;
  auto f = [&seen](const std::pair<HexArray, SHA256Hash> &pair) {
    for_each_func(seen, pair);
  };

  // Insert the KV pairs.
  for (size_t i = 0; i < kNumIterations; i++) {
    HexArray path = generate_random_path(allow_empty);
    std::string value = std::to_string(i);
    std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                    value.size()};
    bool insert = trie.insert(path, value_span);
    EXPECT_EQ(insert, !total.contains(path));
    if (!insert) {
      // A duplicate key was generated, so try again.
      i--;
      continue;
    }

    total.insert({path, value});

    seen.clear();
    trie.for_each(f);
    for (const auto &[k, value_hash] : seen) {
      EXPECT_EQ(value_hash, sha256(total[k]));
    }
    EXPECT_EQ(seen.size(), total.size());

    trie.generate_top_level_hashes();
    const auto &[root_hash, node_encodings] = trie.get_proof(path);
    EXPECT_TRUE(MerkleTrie::prove(root_hash, node_encodings, path, value_span));
    SHA256Hash *value_hash;
    void *cookie;
    EXPECT_TRUE(trie.find_begin(path, value_hash, cookie));
    EXPECT_EQ(*value_hash, sha256(value));
    hogd::MerkleTrie::find_end(cookie);

    EXPECT_TRUE(trie.check());
  }

  // Check every single KV pair.
  for (auto &[path, value] : total) {
    const auto &[root_hash, node_encodings] = trie.get_proof(path);
    std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                    value.size()};
    EXPECT_TRUE(MerkleTrie::prove(root_hash, node_encodings, path, value_span));

    SHA256Hash *value_hash;
    void *cookie;
    EXPECT_TRUE(trie.find_begin(path, value_hash, cookie));
    EXPECT_EQ(*value_hash, sha256(value));
    hogd::MerkleTrie::find_end(cookie);
  }

  // Remove all KV pairs.
  for (size_t i = 0; i < kNumIterations; i++) {
    // We copy [path] from the [total] iterator rather than make [path] a
    // reference because we continue using [path] after removing the the KV
    // pair from [total].
    const HexArray path = total.begin()->first;
    std::string &value = total.begin()->second;
    std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                    value.size()};
    const auto &[root_hash, node_encodings] = trie.get_proof(path);
    EXPECT_TRUE(MerkleTrie::prove(root_hash, node_encodings, path, value_span));

    EXPECT_TRUE(trie.remove(path));
    auto it = total.erase(total.begin());
    if (i < kNumIterations - 1) {
      EXPECT_NE(it, total.end());
    }
    else {
      EXPECT_EQ(it, total.end());
    }

    trie.generate_top_level_hashes();
    EXPECT_TRUE(trie.check());

    SHA256Hash *value_hash;
    void *cookie;
    EXPECT_FALSE(trie.find_begin(path, value_hash, cookie));

    seen.clear();
    trie.for_each(f);
    for (const auto &[k, value_hash] : seen) {
      EXPECT_EQ(value_hash, sha256(total[k]));
    }
    EXPECT_EQ(seen.size(), total.size());

    EXPECT_TRUE(trie.check());
  }
}

} // namespace

TEST(MerkleTrie, Stress)
{
  MerkleTrie trie;
  stress(trie, /*allow_empty=*/true);
}

TEST(MerkleTriePrefix, Stress)
{
  MerkleTriePrefix trie;
  stress(trie, /*allow_empty=*/false);
}

namespace {

// This test first adds 5,000 KV pairs to the trie, then proves each KV pair in
// the trie, then removes all KV pairs.
//
// This is a much larger number of KV pairs than in the [MerkleTrie::Stress]
// test, but we perform fewer checks to reduce test runtime.
template<typename T>
void
stress_two(T &trie, bool allow_empty)
{
  constexpr size_t kNumIterations = 5'000;

  std::map<HexArray, std::string> total;
  std::map<HexArray, SHA256Hash> seen;
  auto f = [&seen](const std::pair<HexArray, SHA256Hash> &pair) {
    for_each_func(seen, pair);
  };

  // Insert the KV pairs.
  for (size_t i = 0; i < kNumIterations; i++) {
    HexArray path = generate_random_path(allow_empty);
    std::string value = std::to_string(i);
    std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                    value.size()};
    bool insert = trie.insert(path, value_span);
    EXPECT_EQ(insert, !total.contains(path));
    if (!insert) {
      // A duplicate key was generated, so try again.
      i--;
      continue;
    }

    total.insert({path, value});
  }

  trie.generate_top_level_hashes();
  EXPECT_TRUE(trie.check());
  // Check every single KV pair.
  for (auto &[path, value] : total) {
    const auto &[root_hash, node_encodings] = trie.get_proof(path);
    std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                    value.size()};
    EXPECT_TRUE(MerkleTrie::prove(root_hash, node_encodings, path, value_span));

    SHA256Hash *value_hash;
    void *cookie;
    EXPECT_TRUE(trie.find_begin(path, value_hash, cookie));
    EXPECT_EQ(*value_hash, sha256(value));
    hogd::MerkleTrie::find_end(cookie);
  }

  // Remove all KV pairs.
  for (size_t i = 0; i < kNumIterations; i++) {
    const auto &[path, _] = *total.begin();

    EXPECT_TRUE(trie.remove(path));
    auto it = total.erase(total.begin());
    if (i < kNumIterations - 1) {
      EXPECT_NE(it, total.end());
    }
    else {
      EXPECT_EQ(it, total.end());
    }
  }

  trie.generate_top_level_hashes();
  EXPECT_TRUE(trie.check());
  EXPECT_EQ(trie.size(), 0);

  seen.clear();
  trie.for_each(f);
  for (const auto &[k, value_hash] : seen) {
    EXPECT_EQ(value_hash, sha256(total[k]));
  }
  EXPECT_EQ(seen.size(), total.size());
}

} // namespace

TEST(MerkleTrie, StressTwo)
{
  MerkleTrie trie;
  stress_two(trie, /*allow_empty=*/true);
}

TEST(MerkleTriePrefix, StressTwo)
{
  MerkleTriePrefix trie;
  stress_two(trie, /*allow_empty=*/false);
}

namespace {

// Inserts and removes ~200,000 KV pairs with [num_threads] threads.
void
run_multithread(size_t num_threads)
{
  constexpr size_t kInitialSize = 10'000;
  // Must be a multiple of 128 since that we test with 128 threads. See the
  // CHECK a few lines below.
  constexpr size_t kNumKVPairs = 200'064;
  const size_t num_kv_pairs_per_thread = kNumKVPairs / num_threads;
  CHECK_EQ(kNumKVPairs % num_threads, 0UL);

  MerkleTriePrefix trie;
  std::vector<std::vector<HexArray>> paths(num_threads);
  {
    std::set<HexArray> generated;
    trie.set_should_hash_nodes_on_each_op(false);

    std::print("Completing initial fill...");
    fflush(stdout);

    for (size_t i = 0; i < kInitialSize; i++) {
      HexArray path = generate_random_path(/*allow_empty=*/false);
      if (generated.contains(path)) {
        i--;
        continue;
      }
      CHECK(generated.insert(path).second);
      std::string value = std::to_string(i);
      std::span<std::byte> value_span{reinterpret_cast<std::byte *>(
                                          value.data()),
                                      value.size()};
      EXPECT_TRUE(trie.insert(path, value_span));
    }
    EXPECT_EQ(trie.size(), kInitialSize);
    trie.set_should_hash_nodes_on_each_op(true);
    trie.generate_all_hashes();
    EXPECT_TRUE(trie.check());

    std::println(" done.");
    std::print("Generating paths for threads...");
    fflush(stdout);

    for (size_t i = 0; i < num_threads; i++) {
      paths[i].reserve(num_kv_pairs_per_thread);

      for (size_t j = 0; j < num_kv_pairs_per_thread; j++) {
        HexArray path = generate_random_path(/*allow_empty=*/false);
        if (generated.contains(path)) {
          j--;
          continue;
        }
        CHECK(generated.insert(path).second);
        paths[i].push_back(path);
      }
    }
  }
  std::println(" done.");
  WorkerPool pool(num_threads);
  std::vector<std::future<void>> futures;
  futures.reserve(num_threads);
  std::string value = "hello";
  std::span<std::byte> value_span{reinterpret_cast<std::byte *>(value.data()),
                                  value.size()};
  std::print("Inserting keys...");
  fflush(stdout);

  for (size_t i = 0; i < num_threads; i++) {
    futures.push_back(pool.submit(
        [i, num_kv_pairs_per_thread, &trie, &paths, &value, &value_span] {
          SHA256Hash *value_hash;
          void *cookie;
          for (size_t j = 0; j < num_kv_pairs_per_thread; j++) {
            EXPECT_TRUE(trie.insert(paths[i][j], value_span));
            EXPECT_TRUE(trie.find_begin(paths[i][j], value_hash, cookie));
            EXPECT_EQ(*value_hash, sha256(value));
            hogd::MerkleTrie::find_end(cookie);
            EXPECT_TRUE(trie.remove(paths[i][j]));
          }
        }));
  }
  for (std::future<void> &future : futures) {
    future.get();
  }

  trie.generate_top_level_hashes();
  std::println(" done.");

  EXPECT_EQ(trie.size(), kInitialSize);
  EXPECT_TRUE(trie.check());
}

} // namespace

TEST(MerkleTriePrefix, Multithread128)
{
  run_multithread(/*num_threads=*/128);
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
