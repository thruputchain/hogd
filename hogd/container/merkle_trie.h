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

  void Print()
  {
    PrintHelper(root_);
    printf("----------\n\n");
  }

  // TODO: For all of the methods below, should they include a mutex as well (in
  // a pair)?

  std::string &operator[](const Key &k)
  {
    static std::string kEmpty = "";

    assert(false);
    return kEmpty;
  }

  SHA256Hash GetRootHash() const { return root_->hash; }

  // TODO: This should probably be the node encodings instead.
  std::vector<std::byte> GetProof(const Key &k)
  {
    Node *node = FindNode(k);
    if (!node) {
      return {};
    }

    std::vector<std::byte> ret;
    while (true) {
      if (!node) {
        break;
      }
      std::vector<std::byte> encoding = GetNodeEncoding(*node);
      ret.insert(ret.end(), encoding.begin(), encoding.end());

      node = node->parent;
    }
    return ret;
  }

  // TODO: Should probably pass node encodings and then walk through those.
  static bool Prove(const MerkleTrie &trie, const SHA256Hash &root_hash,
                    const Key &k, const std::string &value)
  {
    const Node *node = trie.FindNodeConst(k);
    if (!node || node->type != NodeType::kLeaf) {
      return false;
    }
    if (node->value != value) {
      return false;
    }

    const Node *child = nullptr;
    // We just need to verify that we saw a path equal in length to [key]. We do
    // not need to check the path contents because the path contents are already
    // encoded in the hashes. If we reach the root node with all hashes matching
    // up, and we examined a path equal in length to [key], then we verified
    // that the KV pair is in the Merkle Patrica trie.
    int64_t key_length_to_verify = k.size();
    while (true) {
      if (!node) {
        break;
      }
      if (node->hash != GetNodeHash(*node)) {
        return false;
      }

      switch (node->type) {
      case NodeType::kBranch:
        key_length_to_verify -= 1;
        break;
      case NodeType::kExtension:
      case NodeType::kLeaf:
        key_length_to_verify -= node->path.size();
        break;
      }
      child = node;
      node = node->parent;
    }
    assert(!node);
    assert(child);
    // We need to check that (1) we verified the entire key and (2) that
    // [child]'s hash matches the [root_hash]. If both conditions are true,
    // then we verified the KV pair.
    return !key_length_to_verify && child->hash == root_hash;
  }

  std::string *find(const Key &k)
  {
    Node *node = FindNode(k);
    if (node) {
      switch (node->type) {
      case NodeType::kBranch:
      case NodeType::kLeaf:
      {
        // Get the [std::string] out of the [std::optional<std::string>] with
        // the dereference operator.
        std::string &value = *node->value;
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
    size_t idx = 0;
    std::byte branch_hex = std::byte(0x00);
    Node *existing_key = nullptr;
    Node *branch_before_branch = nullptr;
    std::byte branch_before_branch_hex = std::byte(0x00);
    Node *next =
        TraverseToBottom(root_, /*prev_root=*/nullptr,
                         /*last_branch_seen=*/nullptr,
                         /*last_branch_hex_seen=*/std::byte(0x00), /*path=*/k,
                         idx, branch_hex, existing_key, branch_before_branch,
                         branch_before_branch_hex);
    if (existing_key) {
      return false;
    }
    assert(next);
    assert(next->type == NodeType::kBranch);
    assert(idx <= k.size());

    Node *to_hash = nullptr;
    if (idx == k.size()) {
      // The key maps to a branch node, so we should add the value directly into
      // the branch node.
      assert(next->value->empty());
      next->value = std::move(obj);
      next->hash = GetNodeHash(*next);
      to_hash = next;
    }
    else {
      std::byte digit = k.Get(idx);
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
          to_hash = InsertHandleExtensionChild(next->child[branch_idx], next, k,
                                               idx, std::move(obj));
        }
        else {
          assert(next->child[branch_idx]->type == NodeType::kLeaf);
          to_hash = InsertHandleLeafChild(next->child[branch_idx], next, k, idx,
                                          std::move(obj));
        }
      }
      else {
        Node *leaf = CreateLeaf(/*parent=*/next);
        // We are going to use the hex character at [idx] in the key to map to
        // the leaf node from the branch node. Thus, we add 1 below as we do not
        // want to include this character in the path that is stored in the leaf
        // node.
        leaf->path = k.GetSubarray(idx + 1, k.size());
        leaf->value = std::move(obj);
        leaf->hash = GetNodeHash(*leaf);

        next->child[branch_idx] = leaf;
        to_hash = next;
      }
    }

    size_++;
    GenerateHashesFrom(to_hash);
    return true;
  }

  void insert(const Key &k, const T &obj) { insert(k, T(obj)); }

  std::string &insert_or_assign(const Key &k, T &&obj)
  {
    static std::string kEmpty = "";

    assert(false);
    return kEmpty;
  }

  size_t remove(const Key &k)
  {
    // If removing a value from a branch, do nothing.
    //
    // If removing a value in a leaf node, need to look at the parent, which is
    // always a branch node.
    // 1. If the branch node has 2+ *other* branches, just clear the current
    //    branch and then we are done.
    //
    // 2. If the branch node only has 1 *other* branch, or if the branch node
    //    has no *other* branches but does have a value, we can optimize by
    //    removing the branch node. This is slightly tricky.
    //
    //    a) Keep going up the tree until we hit another branch node. Build up
    //    the path as we go up the tree. Specifically, the parent of the branch
    //    may be an extension node that we can remove, too.
    //
    //    b) If the 1 other branch is a leaf, or if there are no other branches
    //    as there is just a value, then remove all of the nodes and add the
    //    built-up path to the leaf.
    //
    //    c) If the 1 other branch is an extension, then remove all of the nodes
    //    and add the built-up path to the extension.
    //
    //    d) If the 1 other branch is a branch, then remove all of the nodes,
    //    replace them with an extension node that has the built-up path, and
    //    then place the "1 other branch" beneath this extension node.
    //
    // 3. If the branch node has no *other* branches, then it must have a value.
    size_t idx = 0;
    std::byte branch_hex = std::byte(0x00);
    Node *existing_key = nullptr;
    Node *branch_before_branch = nullptr;
    std::byte branch_before_branch_hex = std::byte(0x00);
    Node *branch =
        TraverseToBottom(root_, /*prev_root=*/nullptr,
                         /*last_branch_seen=*/nullptr,
                         /*last_branch_hex_seen=*/std::byte(0x00), /*path=*/k,
                         idx, branch_hex, existing_key, branch_before_branch,
                         branch_before_branch_hex);
    if (!existing_key) {
      return 0;
    }
    assert(branch);
    assert(branch->type == NodeType::kBranch);

    Node *to_hash = nullptr;
    if (existing_key == branch) {
      // The key is in a branch.

      assert(branch->value);
      branch->value = std::nullopt;
      to_hash = branch;
    }
    else {
      size_t num_branches = 0;
      for (Node *child : branch->child) {
        if (child) {
          num_branches++;
        }
      }
      assert(num_branches);

      if (num_branches > 2) {
        // Just remove the leaf node from the branch.

        branch->child[std::to_integer<size_t>(branch_hex)] = nullptr;
        // TODO: Fix all frees.
        // delete existing_key;
        to_hash = branch;
      }
      else if (branch_before_branch) {
        Node *cur =
            branch_before_branch
                ->child[std::to_integer<size_t>(branch_before_branch_hex)];
        HexArray cur_path;
        while (true) {
          if (cur->type == NodeType::kBranch) {
            break;
          }
          assert(cur->type == NodeType::kExtension);
          cur_path.Append(cur->path);

          Node *next_cur = cur->child[0];
          // delete cur;
          cur = next_cur;
        }

        branch->child[std::to_integer<size_t>(branch_hex)] = nullptr;
        Node *new_child = nullptr;
        if (num_branches == 1) {
          new_child = CreateLeaf(/*parent=*/branch_before_branch);
          new_child->path = cur_path;
          new_child->value = std::move(branch->value);
          // delete branch;
        }
        else {
          assert(num_branches == 2);

          Node *other_child = nullptr;
          std::byte other_child_hex = std::byte{0x00};
          for (size_t i = 0; i < branch->child.size(); i++) {
            Node *child = branch->child[i];
            if (child) {
              other_child = child;
              other_child_hex = static_cast<std::byte>(i);
              break;
            }
          }
          assert(other_child);

          if (other_child->type == NodeType::kBranch) {
            new_child = CreateExtension(/*parent=*/branch_before_branch);
            cur_path.Append(branch_hex);

            new_child->path = cur_path;
            new_child->child[0] = other_child;
            other_child->parent = new_child;
          }
          else {
            cur_path.Append(other_child_hex);
            cur_path.Append(other_child->path);
            other_child->path = cur_path;

            new_child = other_child;
          }

          // delete branch;
          // delete existing_key;
        }

        branch_before_branch
            ->child[std::to_integer<size_t>(branch_before_branch_hex)] =
            new_child;
        to_hash = new_child;
      }
      else {
        // If [branch_before_branch] is a nullptr, then [branch] must be the
        // root node, because the root node does not have a parent. The root
        // node also never has a value in it.
        assert(branch == root_);
        branch->child[std::to_integer<size_t>(branch_hex)] = nullptr;
        // delete existing_key;
        to_hash = branch;
      }
    }

    size_--;
    GenerateHashesFrom(to_hash);
    return 1;
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
  template<typename F> void for_each(F func)
  {
    HexArray prefix;
    Walk(root_, prefix, func);
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
  enum class NodeType { kBranch, kExtension, kLeaf };

  struct Node {
    NodeType type;
    SHA256Hash hash;
    Node *parent;

    // TODO: Optimize the variables below with a union or different node types.
    // TODO: Optimize the vectors below--use something other than a vector. We
    // know that path lengths will never exceed 65 bytes.

    // Used by extensions and leaves.
    HexArray path;

    // Used by branches and leaves.
    // TODO: Should we be able to place values in extension nodes, too? For
    // example, the [TwoLeavesAndOneBranch] test would produce a more efficient
    // tree if we could do this.
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

  Node *CreateBranch(Node *parent)
  {
    Node *node = new Node;
    node->type = NodeType::kBranch;
    node->parent = parent;
    node->value = std::nullopt;
    memset(node->child.data(), 0, node->child.size() * sizeof(node->child[0]));
    return node;
  }

  Node *CreateExtension(Node *parent)
  {
    Node *node = new Node;
    node->type = NodeType::kExtension;
    node->parent = parent;
    return node;
  }

  Node *CreateLeaf(Node *parent)
  {
    Node *node = new Node;
    node->type = NodeType::kLeaf;
    node->parent = parent;
    node->value = std::nullopt;
    return node;
  }

  // TODO: Move to HexArray class?
  size_t GetLongestCommonRangeLength(const HexArray &a, size_t a_start,
                                     const HexArray &b, size_t b_start)
  {
    assert(a_start <= a.size());
    assert(b_start <= b.size());

    size_t a_size = a.size() - a_start;
    size_t length = 0;
    for (size_t i = 0; i < a_size; i++) {
      if (i + b_start >= b.size()) {
        break;
      }
      if (a.Get(i + a_start) != b.Get(i + b_start)) {
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
      printf("Parent: %p\n", root->parent);
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
      printf("Parent: %p\n", root->parent);
      printf("Path: %s\n", root->path.str().c_str());
      printf("Path Size: %zu\n", root->path.size());
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
      printf("Parent: %p\n", root->parent);
      printf("Path: %s\n", root->path.str().c_str());
      printf("Path Size: %zu\n", root->path.size());
      printf("Value: [%s]\n", root->value ? root->value->c_str() : "<empty>");
      printf("\n");
      break;
    }
  }

  const Node *FindNodeConst(const Key &k) const
  {
    size_t idx = 0;
    std::byte branch_hex = std::byte(0x00);
    Node *existing_key = nullptr;
    Node *branch_before_branch = nullptr;
    std::byte branch_before_branch_hex = std::byte(0x00);
    TraverseToBottomConst(root_, /*prev_root=*/nullptr,
                          /*last_branch_seen=*/nullptr,
                          /*last_branch_hex_seen=*/std::byte(0x00),
                          /*path=*/k, idx, branch_hex, existing_key,
                          branch_before_branch, branch_before_branch_hex);
    return existing_key;
  }

  Node *FindNode(const Key &k) { return const_cast<Node *>(FindNodeConst(k)); }

  // See the callsite for comments on this function.
  Node *InsertHandleExtensionChild(Node *current_extension, Node *parent_branch,
                                   const Key &k, size_t idx, T &&obj)
  {
    // We add 1 since one of the digits will be included in the [parent_branch]
    // node.
    Key key_a = k.GetSubarray(idx + 1, k.size());

    size_t common_range_length =
        GetLongestCommonRangeLength(key_a, 0, current_extension->path, 0);
    assert(common_range_length);
    size_t diverge_idx = common_range_length;

    Node *new_extension_node = CreateExtension(/*parent=*/parent_branch);
    new_extension_node->path = key_a.GetSubarray(0, diverge_idx);

    parent_branch->child[std::to_integer<size_t>(k.Get(idx))] =
        new_extension_node;

    Node *new_branch_node = CreateBranch(/*parent=*/new_extension_node);
    new_extension_node->child[0] = new_branch_node;

    // Move the already-existing extension node underneath the branch node.
    std::byte current_extension_node_branch =
        current_extension->path.Get(diverge_idx);
    new_branch_node
        ->child[std::to_integer<size_t>(current_extension_node_branch)] =
        current_extension;
    current_extension->parent = new_branch_node;

    // Include 1 to account for the branch node, too. [common_range_length] is
    // the size of the path in the extension node, and then we have 1 for the
    // branch node.
    current_extension->path =
        current_extension->path.GetSubarray(diverge_idx + 1,
                                            current_extension->path.size());

    // We hash this extension node here, and then in the parent function, we
    // hash all nodes on the path from the leaf below to the root. Note that
    // this extension node and the leaf below share the same parent.
    current_extension->hash = GetNodeHash(*current_extension);

    // Create new leaf node.
    Node *new_leaf_node = CreateLeaf(/*parent=*/new_branch_node);
    new_leaf_node->path = key_a.GetSubarray(diverge_idx + 1, key_a.size());
    new_leaf_node->value = std::move(obj);

    std::byte leaf_branch = key_a.Get(diverge_idx);
    new_branch_node->child[std::to_integer<size_t>(leaf_branch)] =
        new_leaf_node;
    return new_leaf_node;
  }

  // See the callsite for comments on this function.
  Node *InsertHandleLeafChild(Node *current_leaf, Node *parent_branch,
                              const Key &k, size_t idx, T &&obj)
  {
    // We add 1 since one of the digits will be included in the [parent_branch]
    // node.
    Key key_a = k.GetSubarray(idx + 1, k.size());
    Key key_b = current_leaf->path;
    Node *prev_branch = parent_branch;

    size_t common_range_length =
        GetLongestCommonRangeLength(key_a, 0, key_b, 0);
    if (common_range_length) {
      size_t diverge_idx = common_range_length;
      Node *new_extension_node = CreateExtension(/*parent=*/parent_branch);
      new_extension_node->path = key_a.GetSubarray(0, diverge_idx);

      parent_branch->child[std::to_integer<size_t>(k.Get(idx))] =
          new_extension_node;

      Node *new_branch_node = CreateBranch(/*parent=*/new_extension_node);
      new_extension_node->child[0] = new_branch_node;
      prev_branch = new_branch_node;

      // Now remove the extension path from these keys.
      key_a = key_a.GetSubarray(diverge_idx, key_a.size());
      key_b = current_leaf->path.GetSubarray(diverge_idx,
                                             current_leaf->path.size());
    }

    const Key &shorter = key_a.size() <= key_b.size() ? key_a : key_b;
    const std::string &shorter_value =
        key_a.size() <= key_b.size() ? obj : *current_leaf->value;
    const Key &longer = key_a.size() <= key_b.size() ? key_b : key_a;
    const std::string &longer_value =
        key_a.size() <= key_b.size() ? *current_leaf->value : obj;

    Node *to_hash = nullptr;
    if (shorter.empty()) {
      // The shorter key is entirely contained within the larger key. Thus, we
      // should add the shorter key's value to the last branch node created, and
      // then we should add the longer key and its value to a leaf node of that
      // branch.
      prev_branch->value = std::move(shorter_value);

      // Longer key's leaf node.
      Node *leaf = CreateLeaf(/*parent=*/prev_branch);
      leaf->path = longer.GetSubarray(1, longer.size());
      leaf->value = std::move(longer_value);
      std::byte longer_digit = longer.Get(0);
      prev_branch->child[std::to_integer<size_t>(longer_digit)] = leaf;
      to_hash = leaf;
    }
    else {
      std::byte shorter_digit = shorter.Get(0);
      std::byte longer_digit = longer.Get(0);
      assert(shorter_digit != longer_digit);

      // The digits do not match, so create two leaf nodes.

      // Shorter key's leaf node.
      Node *leaf = CreateLeaf(/*parent=*/prev_branch);
      leaf->path = shorter.GetSubarray(1, shorter.size());
      leaf->value = std::move(shorter_value);
      prev_branch->child[std::to_integer<size_t>(shorter_digit)] = leaf;
      // We hash this leaf here, and then in the parent function, we hash all
      // nodes on the path from the leaf below to the root. Note that this leaf
      // and the leaf below share the same parent.
      leaf->hash = GetNodeHash(*leaf);

      // Longer key's leaf node.
      leaf = CreateLeaf(/*parent=*/prev_branch);
      leaf->path = longer.GetSubarray(1, longer.size());
      leaf->value = std::move(longer_value);
      prev_branch->child[std::to_integer<size_t>(longer_digit)] = leaf;
      to_hash = leaf;
    }
    // delete current_leaf;
    return to_hash;
  }

  template<typename F> void Walk(Node *root, HexArray &prefix, F func)
  {
    // We should never call this function with a null node.
    assert(root);

    switch (root->type) {
    case NodeType::kBranch:
      if (root->value) {
        func({prefix, *root->value});
      }
      for (size_t i = 0; i < root->child.size(); i++) {
        if (root->child[i]) {
          std::byte digit = static_cast<std::byte>(i);

          prefix.Append(digit);
          Walk(root->child[i], prefix, func);
          prefix.RemoveFromBack(1);
        }
      }
      break;
    case NodeType::kExtension:
      assert(root->child[0]);

      prefix.Append(root->path);
      Walk(root->child[0], prefix, func);
      prefix.RemoveFromBack(root->path.size());
      break;
    case NodeType::kLeaf:
      assert(root->value);
      assert((prefix_len % 2) == 0);

      prefix.Append(root->path);
      func({prefix, *root->value});
      prefix.RemoveFromBack(root->path.size());
      break;
    }
  }

  const Node *TraverseToBottomConst(Node *root, Node *prev_root,
                                    Node *last_branch_seen,
                                    std::byte last_branch_hex_seen,
                                    const HexArray &path, size_t &idx,
                                    std::byte &branch_hex, Node *&existing_key,
                                    Node *&branch_before_branch,
                                    std::byte &branch_before_branch_hex) const
  {
    if (!root) {
      return prev_root;
    }
    // There are two hex characters per byte. [idx] is the index of the current
    // hex character we are looking at, so we reach the end of the byte array
    // when [idx >= path.size() * 2].
    if (idx >= path.size()) {
      if (root->type == NodeType::kBranch && !root->value->empty()) {
        existing_key = root;
      }
      return root;
    }
    else if (prev_root && prev_root->type == NodeType::kBranch) {
      branch_before_branch = last_branch_seen;
      last_branch_seen = prev_root;

      branch_before_branch_hex = last_branch_hex_seen;
      last_branch_hex_seen = path.Get(idx - 1);
    }

    std::byte current_hex = path.Get(idx);
    switch (root->type) {
    case NodeType::kBranch:
    {
      Node *next = root->child[std::to_integer<size_t>(current_hex)];
      if (next) {
        idx++;
        branch_hex = current_hex;
        return TraverseToBottomConst(next, root, last_branch_seen,
                                     last_branch_hex_seen, path, idx,
                                     branch_hex, existing_key,
                                     branch_before_branch,
                                     branch_before_branch_hex);
      }
      else {
        // There is no child, so we have reached the bottom of the trie.
        return root;
      }
      break;
    }
    case NodeType::kExtension:
    {
      if (path.GetSubarray(idx, idx + root->path.size()) == root->path) {
        assert(root->child[0]);
        idx += root->path.size();
        return TraverseToBottomConst(root->child[0], root, last_branch_seen,
                                     last_branch_hex_seen, path, idx,
                                     branch_hex, existing_key,
                                     branch_before_branch,
                                     branch_before_branch_hex);
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
      if (path.GetSubarray(idx, path.size()) == root->path) {
        existing_key = root;
      }
      // We are going to return the parent node, so subtract 1 from [idx].
      idx--;
      return prev_root;
      break;
    }
  }

  Node *TraverseToBottom(Node *root, Node *prev_root, Node *last_branch_seen,
                         std::byte last_branch_hex_seen, const HexArray &path,
                         size_t &idx, std::byte &branch_hex,
                         Node *&existing_key, Node *&branch_before_branch,
                         std::byte &branch_before_branch_hex)
  {
    return const_cast<Node *>(
        TraverseToBottomConst(root, prev_root, last_branch_seen,
                              last_branch_hex_seen, path, idx, branch_hex,
                              existing_key, branch_before_branch,
                              branch_before_branch_hex));
  }

  // Regenerates the hashes *only* for the nodes on the path from [n] to the
  // root of the trie.
  void GenerateHashesFrom(Node *n)
  {
    while (true) {
      if (!n) {
        break;
      }

      n->hash = GetNodeHash(*n);
      n = n->parent;
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

  static std::vector<std::byte> GetNodeEncoding(const Node &node)
  {
    static constexpr std::array<std::byte, 32> kNullptr = {};

    // TODO: Optimize. We know the max lengths of the values we need to hash up
    // front.
    std::vector<std::byte> to_hash;
    to_hash.push_back(static_cast<std::byte>(node.type));
    switch (node.type) {
    case NodeType::kBranch:
    {
      std::vector<std::byte> value_size_bytes =
          SizeToBytes(node.value ? node.value->size() : 0);
      to_hash.insert(to_hash.end(), value_size_bytes.begin(),
                     value_size_bytes.end());

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
      std::vector<std::byte> path_size_bytes = SizeToBytes(node.path.size());
      to_hash.insert(to_hash.end(), path_size_bytes.begin(),
                     path_size_bytes.end());

      const std::vector<std::byte> path_bytes = node.path.ToByteVector();
      to_hash.insert(to_hash.end(), path_bytes.begin(), path_bytes.end());

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
      std::vector<std::byte> path_size_bytes = SizeToBytes(node.path.size());
      to_hash.insert(to_hash.end(), path_size_bytes.begin(),
                     path_size_bytes.end());

      const std::vector<std::byte> path_bytes = node.path.ToByteVector();
      to_hash.insert(to_hash.end(), path_bytes.begin(), path_bytes.end());

      std::vector<std::byte> value_size_bytes =
          SizeToBytes(node.value ? node.value->size() : 0);
      to_hash.insert(to_hash.end(), value_size_bytes.begin(),
                     value_size_bytes.end());

      std::transform(node.value->begin(), node.value->end(),
                     std::back_inserter(to_hash), CastCharToByte);
      break;
    }
    }
    return to_hash;
  }

  static SHA256Hash GetNodeHash(const Node &node)
  {
    return sha256(GetNodeEncoding(node));
  }

  void InitRootNode()
  {
    root_ = CreateBranch(/*parent=*/nullptr);
    GenerateHashesFrom(root_);
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
    // delete root;
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
