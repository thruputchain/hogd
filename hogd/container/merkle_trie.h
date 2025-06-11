// merkle.h - provides a merkle patricia trie

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
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
  using Key = std::vector<std::byte>;
  using T = std::string;

  MerkleTrie()
  {
    root_ = new Node;
    root_->type = NodeType::kBranch;
    memset(root_->branch_next.data(), 0,
           root_->branch_next.size() * sizeof(root_->branch_next[0]));
  }
  ~MerkleTrie() { FreeAllNodes(root_); }

  // disable copy and support move.
  MerkleTrie(const MerkleTrie &) = delete;
  MerkleTrie &operator=(const MerkleTrie &) = delete;
  MerkleTrie(MerkleTrie &&) noexcept = default;
  MerkleTrie &operator=(MerkleTrie &&) noexcept = default;

  // TODO: For all of the methods below, should they include a mutex as well (in
  // a pair)?

  std::string &operator[](const Key &key)
  {
    static std::string kEmpty = "";

    assert(false);
    return kEmpty;
  }

  std::string *find(const Key &key)
  {
    static std::string kEmpty = "";

    assert(false);
    return &kEmpty;
  }

  void insert(const Key &k, T &&obj)
  {
    // A root node already exists. If the root is a leaf node, then we need to
    // convert it to a branch node and build the trie.
    //
    // TODO: Optimize with extension nodes.
    // TODO: Handle insertion of a duplicate key.
    size_t idx = 0;
    Node *next =
        TraverseToBottom(root_, /*prev_root=*/nullptr, /*path=*/k, idx);
    assert(next);
    assert(next->type == NodeType::kBranch);
    assert(idx <= k.size() * 2);
    if (idx == k.size()) {
      // The key maps to a branch node, so we should add the value directly into
      // the branch node.
      assert(next->branch_value.empty());
      next->branch_value = std::move(obj);
      next->hash = GetNodeHash(*next);
    }
    else {
      // The key extends beyond the branch node, so we should create a leaf node
      // for the key, add the value to the leaf node, and then add a pointer
      // from the branch node to the leaf node.
      Node *leaf = new Node;
      leaf->type = NodeType::kLeaf;
      // We are going to use the hex character at [idx] in the key to map to the
      // leaf node from the branch node. Thus, we add 1 below as we do not want
      // to include this character in the path that is stored in the leaf node.
      leaf->leaf_path = std::vector(k.begin() + idx + 1, k.end());
      leaf->leaf_value = std::move(obj);
      leaf->hash = GetNodeHash(*leaf);

      size_t branch_idx = std::to_integer<size_t>(k[idx]);
      assert(!next->branch_next[branch_idx]);
      next->branch_next[branch_idx] = leaf;
    }
    RegenerateHashes(root_, k, /*idx=*/0);
  }

  void insert(const Key &k, const T &obj) { assert(false); }

  std::string &insert_or_assign(const Key &k, T &&obj)
  {
    static std::string kEmpty = "";

    assert(false);
    return kEmpty;
  }

  size_t remove(const Key &key)
  {
    assert(false);
    return 0;
  }

  // TODO: Add another [remove] function that also takes a mutex as a parameter.

  size_t size()
  {
    assert(false);
    return 0;
  }

  void clear() { assert(false); }

  // for_each executes func on each element.
  template<typename F> void for_each(F func) { assert(false); }

  // for_each_parallel executes func in parallel on each element using a
  // WorkerPool.
  template<typename F>
  void for_each_parallel(F func, size_t width = kDefaultParallelRangeWidth,
                         WorkerPool &pool = WorkerPool::get_default_pool())
  {
    assert(false);
  }

private:
  enum class NodeType { kBranch, kExtension, kLeaf };

  struct Node {
    NodeType type;
    SHA256Hash hash;

    // TODO: Optimize the variables below with a union or different node types.
    // TODO: Optimize the vectors below--use something other than a vector. We
    // know that path lengths will never exceed 65 bytes.

    // Branches.
    std::array<Node *, 16> branch_next;
    std::string branch_value;

    // Extensions.
    std::vector<std::byte> extension_path;
    Node *extension_key;

    // Leaves.
    std::vector<std::byte> leaf_path;
    std::string leaf_value;
  };

  Node *TraverseToBottom(Node *root, Node *prev_root,
                         const std::vector<std::byte> &path, size_t &idx)
  {
    if (!root) {
      return prev_root;
    }
    // There are two hex characters per byte. [idx] is the index of the current
    // hex character we are looking at, so we reach the end of the byte array
    // when [idx >= path.size() * 2].
    if (idx >= path.size() * 2) {
      return prev_root;
    }

    std::byte current_hex = path[idx / 2];
    if (idx % 2 == 0) {
      current_hex &= std::byte{0xF};
    }
    else {
      current_hex >>= 4;
    }
    switch (root->type) {
    case NodeType::kBranch:
    {
      Node *next = root->branch_next[std::to_integer<size_t>(current_hex)];
      if (next) {
        idx++;
        return TraverseToBottom(next, root, path, idx);
      }
      else {
        // There is no child, so we have reached the bottom of the trie.
        return root;
      }
      break;
    }
    case NodeType::kExtension:
      // TODO: Support this case.
      assert(false);
      return nullptr;
      break;
    case NodeType::kLeaf:
      return prev_root;
      break;
    }
  }

  // Regenerate the hashes in the trie nodes along [path].
  void RegenerateHashes(Node *root, const std::vector<std::byte> &path,
                        size_t idx)
  {
    if (!root) {
      return;
    }
    if (idx >= path.size() * 2) {
      return;
    }

    switch (root->type) {
    case NodeType::kBranch:
      for (Node *child : root->branch_next) {
        RegenerateHashes(child, path, idx + 1);
      }
      break;
    case NodeType::kExtension:
      assert(false);
      // TODO: Implement.
      // RegenerateHashes(root->extension_key, path, idx + 1);
      break;
    case NodeType::kLeaf:
      break;
    }
    root->hash = GetNodeHash(*root);
  }

  SHA256Hash GetNodeHash(const Node &node)
  {
    // TODO: Implement.
    switch (node.type) {
    case NodeType::kBranch:
      break;
    case NodeType::kExtension:
      break;
    case NodeType::kLeaf:
      break;
    }
    static std::array<unsigned char, 32> tmp = {0};
    return tmp;
  }

  void FreeAllNodes(Node *root)
  {
    if (!root) {
      return;
    }

    // We recurse to the bottom of the trie and then free nodes as we move back
    // up. Keys do not exceed 65 bytes in length, so the trie will never have
    // more than 65 * 2 = 130 levels.
    switch (root->type) {
    case NodeType::kBranch:
    {
      for (Node *child : root->branch_next) {
        FreeAllNodes(child);
      }
      break;
    }
    case NodeType::kExtension:
    {
      FreeAllNodes(root->extension_key);
      break;
    }
    case NodeType::kLeaf:
      break;
    }
    delete root;
  }

  Node *root_ = nullptr;
};

} // namespace hogd
