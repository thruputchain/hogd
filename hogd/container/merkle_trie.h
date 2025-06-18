// merkle.h - provides a merkle patricia trie

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cassert>
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

// TODO: Describe the class.
//
// Example:
// TODO: Provide example.
class MerkleTrie {
public:
  // type definitions
  using Key = HexArray;
  using T = std::string;
  using value_type = std::pair<const Key, T>;

  MerkleTrie() { InitRootNode(); }

  ~MerkleTrie() { FreeAllNodes(root_); }

  // disable copy and support move.
  MerkleTrie(const MerkleTrie &) = delete;
  MerkleTrie &operator=(const MerkleTrie &) = delete;
  MerkleTrie(MerkleTrie &&) noexcept = default;
  MerkleTrie &operator=(MerkleTrie &&) noexcept = default;

  std::string str() const;
  friend std::ostream &operator<<(std::ostream &os, const MerkleTrie &trie)
  {
    return os << trie.str();
  }

  // Basic sanity checks on the tree.
  void Check() { CheckHelper(root_, /*parent=*/nullptr); }

  SHA256Hash GetRootHash() const { return root_->hash; }

  std::pair<SHA256Hash, std::vector<std::byte>> GetProof(const Key &k);

  std::pair<SHA256Hash, std::vector<std::vector<std::byte>>>
  GetProofByNode(const Key &k);

  static bool Prove(const std::vector<std::byte> &encodings,
                    const SHA256Hash &root_hash, const Key &k,
                    const std::string &value);

  std::string *find(const Key &k);

  bool insert(const Key &k, T &&obj);
  bool insert(const Key &k, const T &obj) { return insert(k, T(obj)); }

  size_t remove(const Key &k);

  size_t size() { return size_; }

  void clear()
  {
    FreeAllNodes(root_);
    InitRootNode();
    size_ = 0;
  }

  // for_each executes func on each element.
  template<typename F> void for_each(F func)
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
    Walk(root_, prefix, per_node);
  }

  // for_each_parallel executes func in parallel on each element using a
  // WorkerPool.
  template<typename F>
  void for_each_parallel(F func, size_t width = kDefaultParallelRangeWidth,
                         WorkerPool &pool = WorkerPool::get_default_pool())
  {
    assert(false);
  }

private:
  void CheckHelper(Node *root, Node *parent);

  Node *FindNode(const Key &k);

  // See the callsite for comments on this function.
  Node *InsertHandleExtensionChild(Node *current_extension, Node *parent_branch,
                                   const Key &k, size_t idx, T &&obj);

  // See the callsite for comments on this function.
  Node *InsertHandleLeafChild(Node *current_leaf, Node *parent_branch,
                              const Key &k, size_t idx, T &&obj);

  // Does a post-order traversal of the tree starting at [root] with path
  // [prefix], calling [f] on each node.
  //
  // A post-order traversal visits all of the children of a node before visiting
  // the node itself. This is useful when regenerating all of the node hashes in
  // a tree or freeing all of the nodes.
  static void Walk(Node *root, HexArray &prefix,
                   const std::function<void(const HexArray &, Node &)> &f);

  Node *TraverseToBottom(Node *root, Node *prev_root, Node *last_branch_seen,
                         std::byte last_branch_hex_seen, const HexArray &path,
                         size_t &idx, std::byte &branch_hex,
                         Node *&existing_key, Node *&branch_before_branch,
                         std::byte &branch_before_branch_hex);

  // Regenerates the hashes *only* for the nodes on the path from [n] to the
  // root of the trie.
  static void GenerateHashesFrom(Node *n);

  void InitRootNode()
  {
    root_ = Node::CreateBranch(/*parent=*/nullptr);
    GenerateHashesFrom(root_);
  }

  // Frees all of the nodes starting at [root].
  static void FreeAllNodes(Node *root);

  Node *root_ = nullptr;
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
