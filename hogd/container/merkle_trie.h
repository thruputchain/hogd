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
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
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
  using Key = std::vector<std::byte>;
  using T = std::string;

  MerkleTrie()
  {
    root_ = new Node;
    root_->type = NodeType::kBranch;
    memset(root_->branch_next.data(), 0,
           root_->branch_next.size() * sizeof(root_->branch_next[0]));
    root_->hash = GetNodeHash(*root_);
  }

  ~MerkleTrie() { FreeAllNodes(root_); }

  // disable copy and support move.
  MerkleTrie(const MerkleTrie &) = delete;
  MerkleTrie &operator=(const MerkleTrie &) = delete;
  MerkleTrie(MerkleTrie &&) noexcept = default;
  MerkleTrie &operator=(MerkleTrie &&) noexcept = default;

  void Print() { PrintHelper(root_); }

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

    std::string branch_value;
    // Branches. We branch on a hex value. There are 16 different hex values, so
    // we need 16 different branch slots.
    std::array<Node *, 16> branch_next;

    // Extensions.
    std::vector<std::byte> extension_path;
    Node *extension_key;

    // Leaves.
    std::vector<std::byte> leaf_path;
    std::string leaf_value;
  };

  std::string GetHashString(Node &node)
  {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char ch : node.hash) {
      oss << std::setw(2) << static_cast<int>(ch);
    }
    return oss.str();
  }

  void PrintHelper(Node *root)
  {
    if (!root) {
      return;
    }
    switch (root->type) {
    case NodeType::kBranch:
      printf("Branch (%s)\n", GetHashString(*root).c_str());
      printf("Value: %s\n", root->branch_value.c_str());
      for (size_t i = 0; i < root->branch_next.size(); i++) {
        if (root->branch_next[i]) {
          printf("%02x: %s\n", static_cast<uint8_t>(i),
                 GetHashString(*root->branch_next[i]).c_str());
        }
        else {
          printf("%02x: null\n", static_cast<uint8_t>(i));
        }
      }
      for (size_t i = 0; i < root->branch_next.size(); i++) {
        PrintHelper(root->branch_next[i]);
      }
      break;
    case NodeType::kExtension:
      printf("Extension (%s)\n", GetHashString(*root).c_str());
      if (root->extension_key) {
        printf("Child: %s\n", GetHashString(*root->extension_key).c_str());
      }
      else {
        printf("Child: null\n");
      }
      PrintHelper(root->extension_key);
      break;
    case NodeType::kLeaf:
      printf("Leaf (%s)\n", GetHashString(*root).c_str());
      printf("Value: %s\n", root->leaf_value.c_str());
      break;
    }
  }

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

  static std::byte CastCharToByte(unsigned char ch)
  {
    return std::byte(static_cast<unsigned char>(ch));
  }

  static SHA256Hash GetNodeHash(const Node &node)
  {
    static constexpr std::array<std::byte, 32> kNullptr = {};

    // TODO: Optimize. We know the max lengths of the values we need to hash up
    // front.
    std::vector<std::byte> to_hash;
    switch (node.type) {
    case NodeType::kBranch:
      // Hash [branch_value] and **the hashes in** [branch_next].
      std::transform(node.branch_value.begin(), node.branch_value.end(),
                     std::back_inserter(to_hash), CastCharToByte);
      for (Node *child : node.branch_next) {
        if (child) {
          std::transform(child->hash.begin(), child->hash.end(),
                         std::back_inserter(to_hash), CastCharToByte);
        }
        else {
          to_hash.insert(to_hash.end(), kNullptr.begin(), kNullptr.end());
        }
      }
      break;
    case NodeType::kExtension:
      // Hash [extension_path] and **the hash in** [extension_key].
      to_hash.insert(to_hash.end(), node.extension_path.begin(),
                     node.extension_path.end());
      if (node.extension_key) {
        std::transform(node.extension_key->hash.begin(),
                       node.extension_key->hash.end(),
                       std::back_inserter(to_hash), CastCharToByte);
      }
      else {
        to_hash.insert(to_hash.end(), kNullptr.begin(), kNullptr.end());
      }
      break;
    case NodeType::kLeaf:
      // Hash [leaf_path] and [leaf_value].
      to_hash.insert(to_hash.end(), node.leaf_path.begin(),
                     node.leaf_path.end());
      std::transform(node.leaf_value.begin(), node.leaf_value.end(),
                     std::back_inserter(to_hash), CastCharToByte);
      break;
    }
    return sha256(to_hash);
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
      for (Node *child : root->branch_next) {
        FreeAllNodes(child);
      }
      break;
    case NodeType::kExtension:
      FreeAllNodes(root->extension_key);
      break;
    case NodeType::kLeaf:
      break;
    }
    delete root;
  }

  Node *root_ = nullptr;

  FRIEND_TEST(MerkleTrie, HashBranch);
  FRIEND_TEST(MerkleTrie, HashBranchSomeNullptr);
  FRIEND_TEST(MerkleTrie, HashBranchAllNullptr);
  FRIEND_TEST(MerkleTrie, HashExtension);
  FRIEND_TEST(MerkleTrie, HashExtensionNullptr);
  FRIEND_TEST(MerkleTrie, HashLeaf);
};

} // namespace hogd
