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
  using value_type = std::pair<const Key, T>;

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
      case NodeType::kLeaf:
      {
        // Get the [std::string] out of the [std::optional<std::string>] with
        // the dereference operator.
        std::string &value = *existing_key->value;
        return &value;
        break;
      }
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
      assert(next->value->empty());
      next->value = std::move(obj);
      next->hash = GetNodeHash(*next);
    }
    else {
      std::byte digit = HexDigitAtIndex(k, idx);
      size_t branch_idx = std::to_integer<size_t>(digit);
      if (next->child[branch_idx]) {
        // There is already a child node. [TraverseToBottom] returns the lowest
        // branch node in the trie for [key], so the child node is either an
        // extension node or a leaf node.
        //
        // 1. If the child is an extension node, check to see how many hex
        // characters the path and the extension node path have in common.
        // They must have at least one character in common, as otherwise we
        // wouldn't be here. Create a new extension node with the common
        // characters. Then place a branch node node as a child of the new
        // extension node. Then place the already-existing extension node
        // underneath the branch node (with an updated [path]), and
        // also create and place a new leaf node for [k] under the branch node.
        //
        // 2. If the child is a leaf node, check to see what remaining hex
        // characters the key and the leaf node have in common. If there is at
        // least one common character, then we should create an extension node
        // for the common path. Once the hex characters diverge, we should
        // create a branch node. There are two options here:
        //   a) One of the two keys ends on the branch, at which point we can
        //   place this shorter key's value in the branch node and the other
        //   key's value in a leaf of the branch.
        //
        //   b) Neither of the two keys has ended yet. Place each key's value in
        //   its own leaf node under the branch (so there will be two leaf
        //   nodes).
        if (next->child[branch_idx]->type == NodeType::kExtension) {
          InsertHandleExtensionChild(next->child[branch_idx], next, k, idx,
                                     std::move(obj));
        }
        else {
          assert(next->child[branch_idx]->type == NodeType::kLeaf);
          InsertHandleLeafChild(next->child[branch_idx], next, k, idx,
                                std::move(obj));
        }
      }
      else {
        Node *leaf = CreateLeaf();
        // We are going to use the hex character at [idx] in the key to map to
        // the leaf node from the branch node. Thus, we add 1 below as we do not
        // want to include this character in the path that is stored in the leaf
        // node.
        leaf->path = std::vector(k.begin() + (idx + 1) / 2, k.end());
        leaf->path_size = (2 * k.size()) - idx - 1;
        leaf->value = std::move(obj);
        leaf->hash = GetNodeHash(*leaf);

        next->child[branch_idx] = leaf;
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

    // Used by extensions and leaves.
    std::vector<std::byte> path;
    // Used by extensions and leaves.
    //
    // When [path_size] is odd, the first hex character in [path] must still
    // match the hex character in its corresponding position in the full key.
    //
    // For example, if the full key is [0xabcdef], and [path_size] is 3, then
    // [path] must be [0xcdef]. We require the first hex character to still be
    // [c], rather than some garbage value or even [0].
    //
    // This is important so that we consistently produce the same hash for this
    // node. If we did not impose this requirement on the first hex character,
    // then two nodes that are logically equivalent could produce different
    // hashes.
    size_t path_size;

    // Used by branches and leaves.
    std::optional<std::string> value;

    // Used by branches and extensions.
    //
    // For branches, we branch on a hex value and up to all 16 slots may be
    // filled. There are 16 different hex values, so we need 16 different branch
    // slots.
    //
    // For extensions, only the first slot is filled. The other 15 slots are
    // unused.
    std::array<Node *, 16> child;
  };

  Node *CreateBranch()
  {
    Node *node = new Node;
    node->type = NodeType::kBranch;
    node->value = std::nullopt;
    memset(node->child.data(), 0, node->child.size() * sizeof(node->child[0]));
    return node;
  }

  Node *CreateExtension()
  {
    Node *node = new Node;
    node->type = NodeType::kExtension;
    return node;
  }

  Node *CreateLeaf()
  {
    Node *node = new Node;
    node->type = NodeType::kLeaf;
    node->value = std::nullopt;
    return node;
  }

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

  bool HexRangesEqual(const std::vector<std::byte> &a, size_t a_start,
                      size_t a_end, const std::vector<std::byte> &b,
                      size_t b_start, size_t b_end)
  {
    size_t a_size = a_end - a_start;
    size_t b_size = b_end - b_start;
    if (a_size != b_size) {
      return false;
    }
    for (size_t i = 0; i < a_size; i++) {
      if (HexDigitAtIndex(a, i + a_start) != HexDigitAtIndex(b, i + b_start)) {
        return false;
      }
    }
    return true;
  }

  size_t GetLongestCommonRangeLength(const std::vector<std::byte> &a,
                                     size_t a_start,
                                     const std::vector<std::byte> &b,
                                     size_t b_start)
  {
    assert(a_start <= a.size() * 2);
    assert(b_start <= b.size() * 2);

    size_t a_size = a.size() - a_start;
    size_t length = 0;
    for (size_t i = 0; i < a_size * 2; i++) {
      if (i + b_start >= b.size() * 2) {
        break;
      }
      if (HexDigitAtIndex(a, i + a_start) != HexDigitAtIndex(b, i + b_start)) {
        break;
      }
      length++;
    }
    return length;
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
      printf("Value: [%s]\n", root->value ? root->value->c_str() : "<empty>");
      for (size_t i = 0; i < root->child.size(); i++) {
        if (root->child[i]) {
          printf("%x: %s (%p)\n", static_cast<uint8_t>(i),
                 GetHashString(*root->child[i]).c_str(), root->child[i]);
        }
        else {
          printf("%x: null\n", static_cast<uint8_t>(i));
        }
      }
      printf("\n");
      for (size_t i = 0; i < root->child.size(); i++) {
        PrintHelper(root->child[i]);
      }
      break;
    case NodeType::kExtension:
      printf("Extension (%s, %p)\n", GetHashString(*root).c_str(), root);
      printf("Path: %s\n", KeyToString(root->path).c_str());
      printf("Path Size: %zu\n", root->path_size);
      if (root->child[0]) {
        printf("Child: %s (%p)\n", GetHashString(*root->child[0]).c_str(),
               root->child[0]);
      }
      else {
        printf("Child: null\n");
      }
      printf("\n");
      PrintHelper(root->child[0]);
      break;
    case NodeType::kLeaf:
      printf("Leaf (%s, %p)\n", GetHashString(*root).c_str(), root);
      printf("Path: %s\n", KeyToString(root->path).c_str());
      printf("Path Size: %zu\n", root->path_size);
      printf("Value: [%s]\n", root->value ? root->value->c_str() : "<empty>");
      printf("\n");
      break;
    }
  }

  // See the callsite for comments on this function.
  void InsertHandleExtensionChild(Node *current_extension, Node *parent_branch,
                                  const Key &k, size_t idx, T &&obj)
  {
    size_t offset = (idx % 2 == 0) ? 0 : 1;
    Key key_a = std::vector(k.begin() + (idx / 2), k.end());

    size_t common_range_length =
        GetLongestCommonRangeLength(key_a, offset + 1, current_extension->path,
                                    offset + 1);
    assert(common_range_length);
    size_t diverge_idx = offset + 1 + common_range_length;

    Node *new_extension_node = CreateExtension();
    new_extension_node->path = std::vector(key_a.begin() + (offset + 1) / 2,
                                           key_a.begin() + diverge_idx / 2);
    new_extension_node->path_size = common_range_length;

    parent_branch->child[std::to_integer<size_t>(HexDigitAtIndex(k, idx))] =
        new_extension_node;

    Node *new_branch_node = CreateBranch();
    new_extension_node->child[0] = new_branch_node;

    // Move the already-existing extension node underneath the branch node.
    std::byte current_extension_node_branch =
        HexDigitAtIndex(current_extension->path, diverge_idx);
    new_branch_node
        ->child[std::to_integer<size_t>(current_extension_node_branch)] =
        current_extension;

    // Include 1 to account for the branch node, too. [common_range_length] is
    // the size of the path in the extension node, and then we have 1 for the
    // branch node.
    current_extension->path_size -= common_range_length + 1;
    current_extension->path =
        std::vector(current_extension->path.begin() + (diverge_idx / 2),
                    current_extension->path.end());

    // Create new leaf node.
    Node *new_leaf_node = CreateLeaf();
    new_leaf_node->path =
        std::vector(key_a.begin() + diverge_idx / 2, key_a.end());
    new_leaf_node->path_size = key_a.size() * 2 - idx - diverge_idx - 1;
    new_leaf_node->value = std::move(obj);

    std::byte leaf_branch = HexDigitAtIndex(key_a, diverge_idx);
    new_branch_node->child[std::to_integer<size_t>(leaf_branch)] =
        new_leaf_node;
  }

  // See the callsite for comments on this function.
  void InsertHandleLeafChild(Node *current_leaf, Node *parent_branch,
                             const Key &k, size_t idx, T &&obj)
  {
    size_t offset = (idx % 2 == 0) ? 0 : 1;
    Key key_a = std::vector(k.begin() + (idx / 2), k.end());
    Key key_b = current_leaf->path;

    Node *prev_branch = parent_branch;

    size_t common_range_length =
        GetLongestCommonRangeLength(key_a, offset + 1, key_b, offset + 1);
    size_t diverge_idx = offset + 1 + common_range_length;
    if (common_range_length > 1) {
      Node *new_extension_node = CreateExtension();
      new_extension_node->path = std::vector(key_a.begin() + (offset + 1) / 2,
                                             key_a.begin() + diverge_idx / 2);
      new_extension_node->path_size = common_range_length;

      parent_branch
          ->child[std::to_integer<size_t>(HexDigitAtIndex(k, idx + offset))] =
          new_extension_node;

      Node *new_branch_node = CreateBranch();
      new_extension_node->child[0] = new_branch_node;
      prev_branch = new_branch_node;

      // Now remove the extension path from these keys.
      key_a = std::vector(k.begin() + ((idx + diverge_idx) / 2), k.end());
      key_b = std::vector(current_leaf->path.begin() + diverge_idx / 2,
                          current_leaf->path.end());
    }

    const Key &shorter = key_a.size() <= key_b.size() ? key_a : key_b;
    const std::string &shorter_value =
        key_a.size() <= key_b.size() ? obj : *current_leaf->value;
    const Key &longer = key_a.size() <= key_b.size() ? key_b : key_a;
    const std::string &longer_value =
        key_a.size() <= key_b.size() ? *current_leaf->value : obj;

    size_t i;
    // TODO: We probably no longer need a for loop here.
    for (i = 0; i < shorter.size() * 2; i++) {
      std::byte shorter_digit = HexDigitAtIndex(shorter, i + offset);
      std::byte longer_digit = HexDigitAtIndex(longer, i + offset);
      if (shorter_digit == longer_digit) {
        // The digits match, so add a branch node.
        Node *branch = CreateBranch();
        prev_branch->child[std::to_integer<size_t>(shorter_digit)] = branch;
        prev_branch = branch;
      }
      else {
        // The digits do not match, so create two leaf nodes.

        // Shorter key's leaf node.
        Node *leaf = CreateLeaf();
        leaf->path =
            std::vector(shorter.begin() + ((i + offset) / 2), shorter.end());
        leaf->path_size = (shorter.size() * 2) + idx - 1 - (i + offset);
        leaf->value = std::move(shorter_value);
        prev_branch->child[std::to_integer<size_t>(shorter_digit)] = leaf;

        // Longer key's leaf node.
        leaf = CreateLeaf();
        leaf->path =
            std::vector(longer.begin() + ((i + offset) / 2), longer.end());
        leaf->path_size = (longer.size() * 2) + idx - 1 - (i + offset);
        leaf->value = std::move(longer_value);
        prev_branch->child[std::to_integer<size_t>(longer_digit)] = leaf;
        break;
      }
    }
    if (i >= shorter.size() * 2) {
      // The shorter key is entirely contained within the larger key. Thus,
      // we should add the shorter key's value to the last branch node
      // created, and then we should add the longer key and its value to a
      // leaf node of that branch.
      prev_branch->value = std::move(shorter_value);

      // Longer key's leaf node.
      Node *leaf = CreateLeaf();
      leaf->path =
          std::vector(longer.begin() + ((i + offset + 1) / 2), longer.end());
      leaf->path_size = (longer.size() * 2) + idx - (i + offset + 1);
      leaf->value = std::move(longer_value);
      std::byte longer_digit = HexDigitAtIndex(longer, i);
      prev_branch->child[std::to_integer<size_t>(longer_digit)] = leaf;
    }
    delete current_leaf;
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
      // TODO: Find a way to indicate whether [value] is empty or not.
      if (root->type == NodeType::kBranch && !root->value->empty()) {
        existing_key = root;
      }
      return root;
    }

    std::byte current_hex = HexDigitAtIndex(path, idx);
    switch (root->type) {
    case NodeType::kBranch:
    {
      Node *next = root->child[std::to_integer<size_t>(current_hex)];
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
    {
      // Check that path[idx:idx+path_size] and path are
      // equal. If path_size is odd, need to actually compare with
      // path[1:1+path_size].
      size_t offset = root->path_size % 2;
      if (HexRangesEqual(path, idx, idx + root->path_size, root->path, offset,
                         offset + root->path_size)) {
        assert(root->child[0]);
        idx += root->path_size;
        return TraverseToBottom(root->child[0], root, path, idx, existing_key);
      }
      else {
        // The extension node's path does not match [key], so we can stop moving
        // down the tree.
        //
        // We are going to return the parent node, so subtract 1 from [idx].
        idx--;
        return prev_root;
      }
      break;
    }
    case NodeType::kLeaf:
      // We are going to return the parent node, so subtract 1 from [idx].
      idx--;
      if (std::vector(path.begin() + idx / 2, path.end()) == root->path) {
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
      for (Node *child : root->child) {
        RegenerateAllHashes(child);
      }
      break;
    case NodeType::kExtension:
      RegenerateAllHashes(root->child[0]);
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
      // Hash [value] and **the hashes in** [child].
      std::transform(node.value->begin(), node.value->end(),
                     std::back_inserter(to_hash), CastCharToByte);
      for (Node *child : node.child) {
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
      // Hash [path], [path_size], and **the hash in**
      // [child[0]].
      to_hash.insert(to_hash.end(), node.path.begin(), node.path.end());
      std::vector<std::byte> size_bytes = SizeToBytes(node.path_size);
      to_hash.insert(to_hash.end(), size_bytes.begin(), size_bytes.end());
      if (node.child[0]) {
        std::transform(node.child[0]->hash.begin(), node.child[0]->hash.end(),
                       std::back_inserter(to_hash), CastCharToByte);
      }
      else {
        to_hash.insert(to_hash.end(), kNullptr.begin(), kNullptr.end());
      }
      break;
    }
    case NodeType::kLeaf:
    {
      // Hash [path], [path_size], and [value].
      to_hash.insert(to_hash.end(), node.path.begin(), node.path.end());
      std::vector<std::byte> size_bytes = SizeToBytes(node.path_size);
      to_hash.insert(to_hash.end(), size_bytes.begin(), size_bytes.end());
      std::transform(node.value->begin(), node.value->end(),
                     std::back_inserter(to_hash), CastCharToByte);
      break;
    }
    }
    return sha256(to_hash);
  }

  void InitRootNode()
  {
    root_ = CreateBranch();
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
      for (Node *child : root->child) {
        FreeAllNodes(child);
      }
      break;
    case NodeType::kExtension:
      FreeAllNodes(root->child[0]);
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
