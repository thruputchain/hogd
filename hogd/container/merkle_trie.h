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

  MerkleTrie() { InitRootNode(); }

  ~MerkleTrie() { FreeAllNodes(root_); }

  // disable copy and support move.
  MerkleTrie(const MerkleTrie &) = delete;
  MerkleTrie &operator=(const MerkleTrie &) = delete;
  MerkleTrie(MerkleTrie &&) noexcept = default;
  MerkleTrie &operator=(MerkleTrie &&) noexcept = default;

  void Print()
  {
    PrintHelper(root_);
    printf("----------\n\n");
  }

  // TODO: For all of the methods below, should they include a mutex as well (in
  // a pair)?

  std::string &operator[](const Key &key)
  {
    static std::string kEmpty = "";

    assert(false);
    return kEmpty;
  }

  std::string *find(const Key &k)
  {
    size_t idx = 0;
    Node *existing_key = nullptr;
    TraverseToBottom(root_, /*prev_root=*/nullptr, /*path=*/k, idx,
                     existing_key);
    if (existing_key) {
      switch (existing_key->type) {
      case NodeType::kBranch:
        return &existing_key->branch_value;
        break;
      case NodeType::kLeaf:
        return &existing_key->leaf_value;
        break;
      default:
        assert(false);
        return nullptr;
        break;
      }
    }
    return nullptr;
  }

  bool insert(const Key &k, T &&obj)
  {
    // TODO: Optimize with extension nodes.
    size_t idx = 0;
    Node *existing_key = nullptr;
    Node *next = TraverseToBottom(root_, /*prev_root=*/nullptr, /*path=*/k, idx,
                                  existing_key);
    if (existing_key) {
      return false;
    }
    assert(next);
    assert(next->type == NodeType::kBranch);
    assert(idx <= k.size() * 2);
    if (idx == k.size() * 2) {
      // The key maps to a branch node, so we should add the value directly into
      // the branch node.
      assert(next->branch_value.empty());
      next->branch_value = std::move(obj);
      next->hash = GetNodeHash(*next);
    }
    else {
      std::byte digit = HexDigitAtIndex(k, idx);
      size_t branch_idx = std::to_integer<size_t>(digit);
      if (next->branch_next[branch_idx]) {
        Node *to_free = next->branch_next[branch_idx];
        // There is already a child node. [TraverseToBottom] returns the lowest
        // branch node in the trie for [key], so the child node is either an
        // extension node or a leaf node.
        //
        // 1. TODO: If the child is an extension node, ...
        //
        // 2. If the child is a leaf node, we should add one branch node for
        // every remaining character in [key] that matches the path in [leaf].
        // At some point, one of the two will happen:
        //   a) A character will be different, at which point we can place two
        //   leaf nodes below a branch node.
        //
        //   b) One of the two keys will end, at which point we can place the
        //   shorter key in the newly-created branch node and add the longer key
        //   to a leaf node that is a child of that branch node.

        size_t offset = (idx % 2 == 0) ? 0 : 1;
        Key key_a = std::vector(k.begin() + (idx / 2), k.end());
        Key key_b = next->branch_next[branch_idx]->leaf_path;

        const Key &shorter = key_a.size() <= key_b.size() ? key_a : key_b;
        const std::string &shorter_value =
            key_a.size() <= key_b.size()
                ? obj
                : next->branch_next[branch_idx]->leaf_value;
        const Key &longer = key_a.size() <= key_b.size() ? key_b : key_a;
        const std::string &longer_value =
            key_a.size() <= key_b.size()
                ? next->branch_next[branch_idx]->leaf_value
                : obj;
        size_t i;
        Node *prev_branch = next;
        for (i = 0; i < shorter.size() * 2; i++) {
          std::byte shorter_digit = HexDigitAtIndex(shorter, i + offset);
          std::byte longer_digit = HexDigitAtIndex(longer, i + offset);
          if (shorter_digit == longer_digit) {
            // The digits match, so add a branch node.
            Node *branch = new Node;
            branch->type = NodeType::kBranch;
            // TODO: Need some way to indicate [branch_value] is empty.
            branch->branch_value = "";

            memset(branch->branch_next.data(), 0,
                   branch->branch_next.size() * sizeof(branch->branch_next[0]));
            prev_branch->branch_next[std::to_integer<size_t>(shorter_digit)] =
                branch;
            prev_branch = branch;
          }
          else {
            // The digits do not match, so create two leaf nodes.

            // Shorter key's leaf node.
            Node *leaf = new Node;
            leaf->type = NodeType::kLeaf;
            leaf->leaf_path = std::vector(shorter.begin() + ((i + offset) / 2),
                                          shorter.end());
            leaf->leaf_path_size =
                (shorter.size() * 2) + idx - 1 - (i + offset);
            leaf->leaf_value = std::move(shorter_value);
            prev_branch->branch_next[std::to_integer<size_t>(shorter_digit)] =
                leaf;

            // Longer key's leaf node.
            leaf = new Node;
            leaf->type = NodeType::kLeaf;
            leaf->leaf_path =
                std::vector(longer.begin() + ((i + offset) / 2), longer.end());
            leaf->leaf_path_size = (longer.size() * 2) + idx - 1 - (i + offset);
            leaf->leaf_value = std::move(longer_value);
            prev_branch->branch_next[std::to_integer<size_t>(longer_digit)] =
                leaf;
            break;
          }
        }
        if (i >= shorter.size() * 2) {
          // The shorter key is entirely contained within the larger key. Thus,
          // we should add the shorter key's value to the last branch node
          // created, and then we should add the longer key and its value to a
          // leaf node of that branch.
          prev_branch->branch_value = std::move(shorter_value);

          // Longer key's leaf node.
          Node *leaf = new Node;
          leaf->type = NodeType::kLeaf;
          leaf->leaf_path = std::vector(longer.begin() + ((i + offset + 1) / 2),
                                        longer.end());
          leaf->leaf_path_size = (longer.size() * 2) + idx - (i + offset + 1);
          leaf->leaf_value = std::move(longer_value);
          std::byte longer_digit = HexDigitAtIndex(longer, i);
          prev_branch->branch_next[std::to_integer<size_t>(longer_digit)] =
              leaf;
        }
        delete to_free;
      }
      else {
        Node *leaf = new Node;
        leaf->type = NodeType::kLeaf;
        // We are going to use the hex character at [idx] in the key to map to
        // the leaf node from the branch node. Thus, we add 1 below as we do not
        // want to include this character in the path that is stored in the leaf
        // node.
        leaf->leaf_path = std::vector(k.begin() + (idx + 1) / 2, k.end());
        leaf->leaf_path_size = (2 * k.size()) - idx - 1;
        leaf->leaf_value = std::move(obj);
        leaf->hash = GetNodeHash(*leaf);

        next->branch_next[branch_idx] = leaf;
      }
    }
    // TODO: Optimize this to only recompute the hashes on paths that were
    // changed. Note that paths **that do not correspond to the key above**
    // could also be changed!
    RegenerateAllHashes(root_);
    size_++;

    return true;
  }

  void insert(const Key &k, const T &obj) { insert(k, T(obj)); }

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

  size_t size() { return size_; }

  void clear()
  {
    FreeAllNodes(root_);
    InitRootNode();
    size_ = 0;
  }

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
    // When [extension_path_size] is odd, the first hex character in
    // [extension_path] must still match the hex character in its corresponding
    // position in the full key.
    //
    // For example, if the full key is [0xabcdef], and [extension_path_size] is
    // 3, then [extension_path] must be [0xcdef]. We require the first hex
    // character to still be [c], rather than some garbage value or even [0].
    //
    // This is important so that we consistently produce the same hash for this
    // node. If we did not impose this requirement on the first hex character,
    // then two nodes that are logically equivalent could produce different
    // hashes.
    size_t extension_path_size;
    Node *extension_key;

    // Leaves.
    std::vector<std::byte> leaf_path;
    // As above, when [leaf_path_size] is odd, the first hex character in
    // [leaf_path] must still match the hex character in its corresponding
    // position in the full key.
    size_t leaf_path_size;
    std::string leaf_value;
  };

  std::string KeyToString(const Key &k)
  {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::byte b : k) {
      oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
  }

  std::byte HexDigitAtIndex(const std::vector<std::byte> &vec, size_t idx)
  {
    std::byte digit = vec[idx / 2];
    if (idx % 2 == 0) {
      digit >>= 4;
    }
    else {
      digit &= std::byte{0xF};
    }
    return digit;
  }

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
      printf("Branch (%s, %p)\n", GetHashString(*root).c_str(), root);
      printf("Value: [%s]\n", root->branch_value.c_str());
      for (size_t i = 0; i < root->branch_next.size(); i++) {
        if (root->branch_next[i]) {
          printf("%x: %s (%p)\n", static_cast<uint8_t>(i),
                 GetHashString(*root->branch_next[i]).c_str(),
                 root->branch_next[i]);
        }
        else {
          printf("%x: null\n", static_cast<uint8_t>(i));
        }
      }
      printf("\n");
      for (size_t i = 0; i < root->branch_next.size(); i++) {
        PrintHelper(root->branch_next[i]);
      }
      break;
    case NodeType::kExtension:
      printf("Extension (%s, %p)\n", GetHashString(*root).c_str(), root);
      printf("Path: %s\n", KeyToString(root->extension_path).c_str());
      printf("Path Size: %zu\n", root->extension_path_size);
      if (root->extension_key) {
        printf("Child: %s (%p)\n", GetHashString(*root->extension_key).c_str(),
               root->extension_key);
      }
      else {
        printf("Child: null\n");
      }
      printf("\n");
      PrintHelper(root->extension_key);
      break;
    case NodeType::kLeaf:
      printf("Leaf (%s, %p)\n", GetHashString(*root).c_str(), root);
      printf("Path: %s\n", KeyToString(root->leaf_path).c_str());
      printf("Path Size: %zu\n", root->leaf_path_size);
      printf("Value: [%s]\n", root->leaf_value.c_str());
      printf("\n");
      break;
    }
  }

  Node *TraverseToBottom(Node *root, Node *prev_root,
                         const std::vector<std::byte> &path, size_t &idx,
                         Node *&existing_key)
  {
    if (!root) {
      return prev_root;
    }
    // There are two hex characters per byte. [idx] is the index of the current
    // hex character we are looking at, so we reach the end of the byte array
    // when [idx >= path.size() * 2].
    if (idx >= path.size() * 2) {
      // TODO: Find a way to indicate whether [branch_value] is empty or not.
      if (root->type == NodeType::kBranch && !root->branch_value.empty()) {
        existing_key = root;
      }
      return root;
    }

    std::byte current_hex = HexDigitAtIndex(path, idx);
    switch (root->type) {
    case NodeType::kBranch:
    {
      Node *next = root->branch_next[std::to_integer<size_t>(current_hex)];
      if (next) {
        idx++;
        return TraverseToBottom(next, root, path, idx, existing_key);
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
      // We are going to return the parent node, so subtract 1 from [idx].
      idx--;
      if (std::vector(path.begin() + idx / 2, path.end()) == root->leaf_path) {
        existing_key = root;
      }
      return prev_root;
      break;
    }
  }

  // Regenerates all of the hashes in the trie.
  void RegenerateAllHashes(Node *root)
  {
    if (!root) {
      return;
    }

    switch (root->type) {
    case NodeType::kBranch:
      for (Node *child : root->branch_next) {
        RegenerateAllHashes(child);
      }
      break;
    case NodeType::kExtension:
      assert(false);
      RegenerateAllHashes(root->extension_key);
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

  static std::vector<std::byte> SizeToBytes(size_t s)
  {
    std::vector<std::byte> ret;
    for (size_t i = 0; i < sizeof(size_t); i++) {
      ret.push_back(std::byte((s >> (8 * i)) & 0xFF));
    }
    return ret;
  }

  static SHA256Hash GetNodeHash(const Node &node)
  {
    static constexpr std::array<std::byte, 32> kNullptr = {};

    // TODO: Optimize. We know the max lengths of the values we need to hash up
    // front.
    std::vector<std::byte> to_hash;
    switch (node.type) {
    case NodeType::kBranch:
    {
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
    }
    case NodeType::kExtension:
    {
      // Hash [extension_path], [extension_path_size], and **the hash in**
      // [extension_key].
      to_hash.insert(to_hash.end(), node.extension_path.begin(),
                     node.extension_path.end());
      std::vector<std::byte> size_bytes = SizeToBytes(node.extension_path_size);
      to_hash.insert(to_hash.end(), size_bytes.begin(), size_bytes.end());
      if (node.extension_key) {
        std::transform(node.extension_key->hash.begin(),
                       node.extension_key->hash.end(),
                       std::back_inserter(to_hash), CastCharToByte);
      }
      else {
        to_hash.insert(to_hash.end(), kNullptr.begin(), kNullptr.end());
      }
      break;
    }
    case NodeType::kLeaf:
    {
      // Hash [leaf_path], [leaf_path_size], and [leaf_value].
      to_hash.insert(to_hash.end(), node.leaf_path.begin(),
                     node.leaf_path.end());
      std::vector<std::byte> size_bytes = SizeToBytes(node.leaf_path_size);
      to_hash.insert(to_hash.end(), size_bytes.begin(), size_bytes.end());
      std::transform(node.leaf_value.begin(), node.leaf_value.end(),
                     std::back_inserter(to_hash), CastCharToByte);
      break;
    }
    }
    return sha256(to_hash);
  }

  void InitRootNode()
  {
    root_ = new Node;
    root_->type = NodeType::kBranch;
    memset(root_->branch_next.data(), 0,
           root_->branch_next.size() * sizeof(root_->branch_next[0]));
    // TODO: Need some way to indicate [branch_value] is empty.
    root_->hash = GetNodeHash(*root_);
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
  size_t size_ = 0;

  FRIEND_TEST(MerkleTrie, HashBranch);
  FRIEND_TEST(MerkleTrie, HashBranchSomeNullptr);
  FRIEND_TEST(MerkleTrie, HashBranchAllNullptr);
  FRIEND_TEST(MerkleTrie, HashExtension);
  FRIEND_TEST(MerkleTrie, HashExtensionNullptr);
  FRIEND_TEST(MerkleTrie, HashLeaf);
};

} // namespace hogd
