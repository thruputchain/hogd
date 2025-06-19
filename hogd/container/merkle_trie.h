// merkle.h - provides a merkle patricia trie

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hex_array.h"
#include "hogd/base/check.h"
#include "hogd/base/compiler.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/node.h"
#include "hogd/crypto/crypto.h"
#include "types.hh"

// TODO: Support template types. For now, the keys are byte arrays and the
// values are strings.
namespace hogd {

// This is a Merkle Patricia Trie. It holds KV pairs and can efficiently
// prove/verify whether a KV pair is in the trie.
//
// Example:
// MerkleTrie trie;
//
// HexArray key("1234");
// std::string value = "hello";
// trie.Insert(key, value);
//
// std::pair<SHA256Hash, std::vector<std::byte>> proof = trie.GetProof();
//
// This returns true:
// trie.Prove(proof.first, proof.second, key, value);
//
// This returns false:
// HexArray different_key("12");
// trie.Prove(proof.first, proof.second, different_key, value);
//
// trie.Remove(key);
class MerkleTrie {
public:
  // type definitions
  using Key = HexArray;
  using T = std::string;
  using value_type = std::pair<const Key, T>;

  MerkleTrie() { InitRootNode(); }

  ~MerkleTrie() { FreeAllNodes(&root_); }

  // Disable copy and support move.
  MerkleTrie(const MerkleTrie &) = delete;
  MerkleTrie &operator=(const MerkleTrie &) = delete;
  MerkleTrie(MerkleTrie &&) noexcept = default;
  MerkleTrie &operator=(MerkleTrie &&) noexcept = default;

  std::string str() const;
  friend std::ostream &operator<<(std::ostream &os, const MerkleTrie &trie)
  {
    return os << trie.str();
  }

  // Basic sanity checks on the trie.
  bool Check() { return CheckHelper(&root_, /*parent=*/nullptr); }

  SHA256Hash GetRootHash() const { return root_.hash; }

  std::pair<SHA256Hash, std::vector<std::byte>> GetProof(const Key &k);

  std::pair<SHA256Hash, std::vector<std::vector<std::byte>>>
  GetProofByNode(const Key &k);

  static bool Prove(const SHA256Hash &root_hash,
                    const std::vector<std::byte> &encodings, const Key &k,
                    const std::string &value);

  std::string *Find(const Key &k);

  bool Insert(const Key &k, T &&obj);
  bool Insert(const Key &k, const T &obj) { return Insert(k, T(obj)); }

  bool Remove(const Key &k);

  size_t size() { return size_; }

  void Clear()
  {
    FreeAllNodes(&root_);
    InitRootNode();
    size_ = 0;
  }

  // Executes [func] on each element.
  template<typename F> void ForEach(F func)
  {
    HexArray prefix;
    auto per_node = [&func](const HexArray &path, Node &node) {
      switch (node.type) {
      case Node::Type::kBranch:
      case Node::Type::kLeaf:
        if (node.value) {
          func({path, *node.value});
        }
        break;
      default:
        break;
      }
    };
    Walk<Node>(&root_, prefix, per_node);
  }

  // Executes [func] in parallel on each element using a WorkerPool.
  template<typename F>
  void ForEachParallel(F func, size_t width = kDefaultParallelRangeWidth,
                       WorkerPool &pool = WorkerPool::get_default_pool())
  {
    CHECK(false);
  }

private:
  static Node *RemoveBranch(const Key &k, Node *branch, size_t num_children);

  static bool CheckHelper(Node *root, Node *parent);

  Node *FindNode(const Key &k);

  // See the implementation for comments on this function.
  static Node *InsertHandleExtension(Node *current_extension, const Key &k,
                                     size_t idx, T &&obj);

  // See the implementation for comments on this function.
  static Node *InsertHandleLeaf(Node *current_leaf, const Key &k, size_t idx,
                                T &&obj);

  static std::string StringFromNode(const Node *root);

  // Does a post-order traversal of the trie starting at [root] with path
  // [prefix], calling [f] on each node.
  //
  // A post-order traversal visits all of the children of a node before visiting
  // the node itself. This is useful when regenerating all of the node hashes in
  // a trie or freeing all of the nodes.
  //
  // [NodeType] must either be [Node] or [const Node].
  template<typename NodeType>
  static void Walk(NodeType *root, HexArray &prefix,
                   const std::function<void(const HexArray &, NodeType &)> &f);

  // Returns the lowest branch in the trie on [path]. [idx] is set to the index
  // of the hex digit in [path] that corresponds to the lowest branch.
  //
  // If a node for [path] already exists in the trie, [existing_key] is set to
  // the node. Thus, since a KV pair can only be placed in a branch or a leaf,
  // [existing_key] is only set to nodes of these types. Regardless of whether
  // this node exist, the function always returns the lowest branch in the trie
  // on [path]. Note that [existing_key] should be a [nullptr] when this
  // function is called so that the caller can determine whether [existing_key]
  // is set to a node.
  static Node *TraverseToLowestBranch(const HexArray &path, Node *root,
                                      Node *prev_branch, size_t &idx,
                                      Node *&existing_key);

  // Regenerates the hashes *only* for the nodes on the path from [n] to the
  // root of the trie.
  static void GenerateHashesFrom(Node *n);

  void InitRootNode()
  {
    root_.Clear();
    root_.InitBranch(/*parent=*/nullptr);
    GenerateHashesFrom(&root_);
  }

  // Frees all of the nodes starting at [root].
  void FreeAllNodes(Node *root);

  // The root node in the trie. The root node always exists and is always a
  // branch.
  Node root_;
  // The number of KV pairs in the trie.
  size_t size_ = 0;

  // TODO: How many of these are still necessary?
  FRIEND_TEST(MerkleTrie, TwoLeavesAndOneBranchProve);
  FRIEND_TEST(MerkleTrie, HashBranch);
  FRIEND_TEST(MerkleTrie, HashBranchSomeNullptr);
  FRIEND_TEST(MerkleTrie, HashBranchAllNullptr);
  FRIEND_TEST(MerkleTrie, HashExtension);
  FRIEND_TEST(MerkleTrie, HashExtensionNullptr);
  FRIEND_TEST(MerkleTrie, HashLeaf);
  FRIEND_TEST(MerkleTrie, Stress);
};

} // namespace hogd
