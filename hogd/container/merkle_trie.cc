#include "hogd/container/merkle_trie.h"

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

namespace hogd {

std::string
MerkleTrie::str() const
{
  HexArray prefix;
  std::ostringstream oss;
  auto per_node = [&oss](const HexArray &path, Node &node) {
    oss << node << std::endl;
  };
  Walk(root_, prefix, per_node);
  oss << "----------" << std::endl;
  return oss.str();
}

std::pair<SHA256Hash, std::vector<std::vector<std::byte>>>
MerkleTrie::GetProofByNode(const Key &k)
{
  Node *node = FindNode(k);
  if (!node) {
    return {SHA256Hash(), {}};
  }

  std::vector<std::vector<std::byte>> ret;
  while (true) {
    if (!node) {
      break;
    }
    std::vector<std::byte> encoding = node->GetNodeEncoding();
    ret.push_back(encoding);

    node = node->parent;
  }
  return {GetRootHash(), ret};
}

std::pair<SHA256Hash, std::vector<std::byte>>
MerkleTrie::GetProof(const Key &k)
{
  std::pair<SHA256Hash, std::vector<std::vector<std::byte>>> proof =
      GetProofByNode(k);
  std::pair<SHA256Hash, std::vector<std::byte>> ret;
  ret.first = proof.first;
  for (const std::vector<std::byte> &node_encoding : proof.second) {
    ret.second.insert(ret.second.end(), node_encoding.begin(),
                      node_encoding.end());
  }
  return ret;
}

// static
bool
MerkleTrie::Prove(const std::vector<std::byte> &encodings,
                  const SHA256Hash &root_hash, const Key &k,
                  const std::string &value)
{
  Node::Raw node;
  memset(&node.hash, 0, sizeof(node.hash));

  size_t num_nodes = 0;
  size_t idx = 0;
  // We just need to verify that we saw a path equal in length to [key]. We do
  // not need to check the path contents themselves because the path contents
  // are already included in the hashes. If the root hash we generate from the
  // [encodings] matches the [root_hash] passed in **and** we examined a path
  // equal in length to [k], then we verified that the KV pair is in the trie.
  size_t verified_key_size = 0;
  while (idx < encodings.size()) {
    SHA256Hash prev_hash = node.hash;
    if (!Node::GetNodeFromEncoding(encodings, idx, node)) {
      return false;
    }

    bool ok;
    switch (node.type) {
    case Node::Type::kBranch:
    {
      if (num_nodes) {
        ok = true;
        verified_key_size++;

        size_t num_matches = 0;
        for (SHA256Hash child_hash : node.child_hashes) {
          if (child_hash == prev_hash) {
            num_matches++;
          }
        }
        ok = ok && (num_matches == 1);
      }
      else {
        // We could start on a branch if the value is directly inside the
        // branch.
        ok = (node.value == value);
      }
      break;
    }
    case Node::Type::kExtension:
      ok = num_nodes;
      verified_key_size += node.path.size();
      ok = ok && (node.child_hashes[0] == prev_hash);
      break;
    case Node::Type::kLeaf:
      ok = !num_nodes;
      verified_key_size += node.path.size();
      ok = ok && (node.value == value);
      break;

    default:
      assert(false);
      return false;
      break;
    }
    if (!ok) {
      return false;
    }
    num_nodes++;
  }
  // [idx] should never be greater than [encodings.size()] if we reach this
  // point.
  assert(idx == encodings.size());
  if (!num_nodes) {
    return false;
  }
  // TODO: Need to check that the root node is a branch?

  // Check that we verified the entire key and that the root hash matches
  // up.
  return verified_key_size == k.size() && node.hash == root_hash;
}

std::string *
MerkleTrie::find(const Key &k)
{
  Node *node = FindNode(k);
  if (node) {
    switch (node->type) {
    case Node::Type::kBranch:
      // Check if the branch has a value.
      if (node->value) {
        // Get the [std::string] out of the [std::optional<std::string>] with
        // the dereference operator.
        std::string &value = *node->value;
        return &value;
      }
      return nullptr;

    case Node::Type::kLeaf:
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

bool
MerkleTrie::insert(const Key &k, T &&obj)
{
  size_t idx = 0;
  std::byte branch_hex = std::byte(0x00);
  Node *existing_key = nullptr;
  Node *branch_before_branch = nullptr;
  std::byte branch_before_branch_hex = std::byte(0x00);
  Node *next = TraverseToBottom(root_, /*prev_root=*/nullptr,
                                /*last_branch_seen=*/nullptr,
                                /*last_branch_hex_seen=*/std::byte(0x00),
                                /*path=*/k, idx, branch_hex, existing_key,
                                branch_before_branch, branch_before_branch_hex);
  if (existing_key) {
    return false;
  }
  assert(next);
  assert(next->type == Node::Type::kBranch);
  assert(idx <= k.size());

  Node *to_hash = nullptr;
  if (idx == k.size()) {
    // The key maps to a branch node, so we should add the value directly into
    // the branch node.
    assert(next->value->empty());
    next->value = std::move(obj);
    next->hash = next->GetNodeHash();
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
      if (next->child[branch_idx]->type == Node::Type::kExtension) {
        to_hash = InsertHandleExtensionChild(next->child[branch_idx], next, k,
                                             idx, std::move(obj));
      }
      else {
        assert(next->child[branch_idx]->type == Node::Type::kLeaf);
        to_hash = InsertHandleLeafChild(next->child[branch_idx], next, k, idx,
                                        std::move(obj));
      }
    }
    else {
      Node *leaf = Node::CreateLeaf(/*parent=*/next);
      // We are going to use the hex character at [idx] in the key to map to
      // the leaf node from the branch node. Thus, we add 1 below as we do not
      // want to include this character in the path that is stored in the leaf
      // node.
      leaf->path = k.GetSubarray(idx + 1, k.size());
      leaf->value = std::move(obj);
      leaf->hash = leaf->GetNodeHash();

      next->child[branch_idx] = leaf;
      to_hash = next;
    }
  }

  size_++;
  GenerateHashesFrom(to_hash);
  return true;
}

size_t
MerkleTrie::remove(const Key &k)
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
  assert(branch->type == Node::Type::kBranch);

  Node *to_hash = nullptr;
  if (existing_key == branch) {
    // The key is in a branch.
    assert(branch->value);
    branch->value = std::nullopt;
    to_hash = branch;

    // TODO: Handle the case where the branch only has one child, and so now
    // the branch is no longer needed. I should probably do this when I clean
    // the rest of the code up.
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
      // We are going to remove the branch node.

      Node *cur =
          branch_before_branch
              ->child[std::to_integer<size_t>(branch_before_branch_hex)];
      HexArray cur_path;
      // TODO: There should only be one extension node at most, so we probably
      // do not need a while loop.
      while (true) {
        if (cur->type == Node::Type::kBranch) {
          break;
        }
        assert(cur->type == Node::Type::kExtension);
        cur_path.Append(cur->path);

        Node *next_cur = cur->child[0];
        // delete cur;
        cur = next_cur;
      }

      // cur_path.Append(branch_hex);
      if (existing_key != branch) {
        branch->child[std::to_integer<size_t>(branch_hex)] = nullptr;
      }

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

      Node *new_child = nullptr;
      if (num_branches == 1) {
        if (!other_child) {

          // TODO: What if the parent is an extension node?
          if (branch->value.has_value()) {
            // The branch has a value, so the place the value in a leaf with
            // path size 0.
            new_child = Node::CreateLeaf(/*parent=*/branch_before_branch);
            new_child->path = cur_path;
            new_child->value = std::move(branch->value);
          }
        }
        else {
          cur_path.Append(other_child_hex);
          new_child = Node::CreateLeaf(/*parent=*/branch_before_branch);
          new_child->path = cur_path;
          new_child->value = std::move(branch->value);
        }
        // delete branch;
      }
      else {
        assert(num_branches == 2);

        if (other_child->type == Node::Type::kBranch) {
          new_child = Node::CreateExtension(/*parent=*/branch_before_branch);
          cur_path.Append(other_child_hex);
          new_child->path = cur_path;
          if (new_child->path.size() == 0) {
          }
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
      if (new_child) {
        new_child->parent = branch_before_branch;
        to_hash = new_child;
      }
      else {
        to_hash = branch_before_branch;
      }
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

void
MerkleTrie::CheckHelper(Node *root, Node *parent)
{
  if (!root) {
    return;
  }
  if (root->parent != parent) {
    printf("The node's parent pointer does not match its actual parent.\n");
  }

  switch (root->type) {
  case Node::Type::kBranch:
  {
    if (root->path.size()) {
      printf("Unexpected path in branch: [%s]\n", root->path.str().c_str());
    }
    size_t num_children = 0;
    for (Node *child : root->child) {
      if (child) {
        num_children++;
      }
      CheckHelper(child, root);
    }
    if (!num_children) {
      printf("Branch node has no children\n");
    }
    if (!root->value && num_children == 1) {
      // Check if this is the root node. The root node is a branch that is
      // allowed to have no children.
      if (root->parent) {
        printf("Branch node has no value and only one child\n");
      }
    }
    break;
  }
  case Node::Type::kExtension:
    if (!root->path.size()) {
      printf("Unexpected empty path in extension: [%s]\n",
             root->path.str().c_str());
    }
    if (root->child[0]) {
      if (root->child[0]->type == Node::Type::kExtension) {
        printf("Extension node has extension node as child\n");
      }
      else if (root->child[0]->type == Node::Type::kLeaf) {
        printf("Extension node has leaf node as child\n");
      }
    }
    else {
      printf("Extension node has no child\n");
    }
    CheckHelper(root->child[0], root);
    break;
  case Node::Type::kLeaf:
    if (!root->value) {
      printf("Unexpected empty value in leaf\n");
    }
    break;
  default:
    printf("Unknown node type: %d\n", static_cast<int>(root->type));
    break;
  }
}

namespace {

// TODO: Move to HexArray class?
size_t
GetLongestCommonRangeLength(const HexArray &a, size_t a_start,
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

} // namespace

Node *
MerkleTrie::FindNode(const Key &k)
{
  size_t idx = 0;
  std::byte branch_hex = std::byte(0x00);
  Node *existing_key = nullptr;
  Node *branch_before_branch = nullptr;
  std::byte branch_before_branch_hex = std::byte(0x00);
  TraverseToBottom(root_, /*prev_root=*/nullptr,
                   /*last_branch_seen=*/nullptr,
                   /*last_branch_hex_seen=*/std::byte(0x00),
                   /*path=*/k, idx, branch_hex, existing_key,
                   branch_before_branch, branch_before_branch_hex);
  return existing_key;
}

// See the callsite for comments on this function.
Node *
MerkleTrie::InsertHandleExtensionChild(Node *current_extension,
                                       Node *parent_branch, const Key &k,
                                       size_t idx, T &&obj)
{
  // We add 1 since one of the digits will be included in the [parent_branch]
  // node.
  Key key_a = k.GetSubarray(idx + 1, k.size());
  assert(key_a.size());

  size_t common_range_length =
      GetLongestCommonRangeLength(key_a, 0, current_extension->path, 0);
  assert(common_range_length);
  size_t diverge_idx = common_range_length;

  // TODO: This extension node could have a path length of 0. We should omit
  // the extension node in that case.
  Node *new_branch_node;
  if (diverge_idx) {
    Node *new_extension_node = Node::CreateExtension(/*parent=*/parent_branch);
    new_extension_node->path = key_a.GetSubarray(0, diverge_idx);

    parent_branch->child[std::to_integer<size_t>(k.Get(idx))] =
        new_extension_node;

    new_branch_node = Node::CreateBranch(/*parent=*/new_extension_node);
    new_extension_node->child[0] = new_branch_node;
  }
  else {
    new_branch_node = Node::CreateBranch(/*parent=*/parent_branch);
    parent_branch->child[std::to_integer<size_t>(k.Get(idx))] = new_branch_node;
  }

  // Move the already-existing extension node underneath the branch node.
  // TODO: Similarly, this extension node could have a path length of 0. We
  // should place the extension node's child directly into the branch node in
  // this case.
  std::byte current_extension_node_branch =
      current_extension->path.Get(diverge_idx);
  if (diverge_idx + 1 < current_extension->path.size()) {
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
    current_extension->hash = current_extension->GetNodeHash();
  }
  else {
    new_branch_node
        ->child[std::to_integer<size_t>(current_extension_node_branch)] =
        current_extension->child[0];
    current_extension->child[0]->parent = new_branch_node;
  }
  Node *new_node;
  if (diverge_idx >= key_a.size()) {
    // Place directly into the branch.
    new_branch_node->value = std::move(obj);
    new_node = new_branch_node;
  }
  else {
    // Create new leaf node.
    Node *new_leaf_node = Node::CreateLeaf(/*parent=*/new_branch_node);
    new_leaf_node->path = key_a.GetSubarray(diverge_idx + 1, key_a.size());
    new_leaf_node->value = std::move(obj);

    std::byte leaf_branch = key_a.Get(diverge_idx);
    new_branch_node->child[std::to_integer<size_t>(leaf_branch)] =
        new_leaf_node;
    new_node = new_leaf_node;
  }
  return new_node;
}

// See the callsite for comments on this function.
Node *
MerkleTrie::InsertHandleLeafChild(Node *current_leaf, Node *parent_branch,
                                  const Key &k, size_t idx, T &&obj)
{
  // We add 1 since one of the digits will be included in the [parent_branch]
  // node.
  std::byte parent_branch_digit = k.Get(idx);
  Key key_a = k.GetSubarray(idx + 1, k.size());
  Key key_b = current_leaf->path;

  Node *new_branch_node = Node::CreateBranch(/*parent=*/parent_branch);
  parent_branch->child[std::to_integer<size_t>(parent_branch_digit)] =
      new_branch_node;

  size_t common_range_length = GetLongestCommonRangeLength(key_a, 0, key_b, 0);
  // printf("common_range_length = %zu, parent_branch = %p\n",
  // common_range_length, parent_branch);
  if (common_range_length) {
    size_t diverge_idx = common_range_length;
    Node *new_extension_node = Node::CreateExtension(/*parent=*/parent_branch);
    new_extension_node->path = key_a.GetSubarray(0, diverge_idx);
    if (new_extension_node->path.size() == 0) {
      printf("YIKES HERE C\n");
    }

    parent_branch->child[std::to_integer<size_t>(parent_branch_digit)] =
        new_extension_node;
    new_extension_node->child[0] = new_branch_node;
    new_branch_node->parent = new_extension_node;

    // Now remove the extension path from these keys.
    key_a = key_a.GetSubarray(diverge_idx, key_a.size());
    key_b =
        current_leaf->path.GetSubarray(diverge_idx, current_leaf->path.size());
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
    new_branch_node->value = std::move(shorter_value);

    // Longer key's leaf node.
    Node *leaf = Node::CreateLeaf(/*parent=*/new_branch_node);
    leaf->path = longer.GetSubarray(1, longer.size());
    leaf->value = std::move(longer_value);
    new_branch_node->child[std::to_integer<size_t>(longer.Get(0))] = leaf;
    to_hash = leaf;
  }
  else {
    assert(shorter.Get(0) != longer.Get(0));

    // The digits do not match, so create two leaf nodes.

    // Shorter key's leaf node.
    Node *leaf = Node::CreateLeaf(/*parent=*/new_branch_node);
    leaf->path = shorter.GetSubarray(1, shorter.size());
    leaf->value = std::move(shorter_value);
    new_branch_node->child[std::to_integer<size_t>(shorter.Get(0))] = leaf;
    // We hash this leaf here, and then in the parent function, we hash all
    // nodes on the path from the leaf below to the root. Note that this leaf
    // and the leaf below share the same parent.
    leaf->hash = leaf->GetNodeHash();

    // Longer key's leaf node.
    leaf = Node::CreateLeaf(/*parent=*/new_branch_node);
    leaf->path = longer.GetSubarray(1, longer.size());
    leaf->value = std::move(longer_value);
    new_branch_node->child[std::to_integer<size_t>(longer.Get(0))] = leaf;
    to_hash = leaf;
  }
  // delete current_leaf;
  return to_hash;
}

// static
void
MerkleTrie::Walk(Node *root, HexArray &prefix,
                 const std::function<void(const HexArray &, Node &)> &f)
{
  if (!root) {
    return;
  }

  switch (root->type) {
  case Node::Type::kBranch:
    for (size_t i = 0; i < root->child.size(); i++) {
      std::byte digit = static_cast<std::byte>(i);

      prefix.Append(digit);
      Walk(root->child[i], prefix, f);
      prefix.RemoveFromBack(1);
    }
    f(prefix, *root);
    break;
  case Node::Type::kExtension:
    assert(root->child[0]);

    prefix.Append(root->path);
    Walk(root->child[0], prefix, f);
    f(prefix, *root);
    prefix.RemoveFromBack(root->path.size());
    break;
  case Node::Type::kLeaf:
    prefix.Append(root->path);
    f(prefix, *root);
    prefix.RemoveFromBack(root->path.size());
    break;
  }
}

Node *
MerkleTrie::TraverseToBottom(Node *root, Node *prev_root,
                             Node *last_branch_seen,
                             std::byte last_branch_hex_seen,
                             const HexArray &path, size_t &idx,
                             std::byte &branch_hex, Node *&existing_key,
                             Node *&branch_before_branch,
                             std::byte &branch_before_branch_hex)
{
  if (!root) {
    return prev_root;
  }
  // There are two hex characters per byte. [idx] is the index of the current
  // hex character we are looking at, so we reach the end of the byte array
  // when [idx >= path.size() * 2].
  if (idx >= path.size()) {
    if (root->type == Node::Type::kBranch) {
      // printf("ret special case A\n");
      if (!root->value->empty()) {
        existing_key = root;
      }
      return root;
    }
    else if (prev_root && prev_root->type == Node::Type::kBranch) {
      // printf("ret special case B, root = %p, prev_root = %p\n", root,
      //    prev_root);
      if (root->type == Node::Type::kLeaf && root->path.size() == 0) {
        // printf("yep here!!!!!!\n");
        existing_key = root;

        branch_before_branch = last_branch_seen;
        branch_before_branch_hex = last_branch_hex_seen;
      }
      idx--;
      return prev_root;
    }
  }
  else if (prev_root && prev_root->type == Node::Type::kBranch) {
    branch_before_branch = last_branch_seen;
    last_branch_seen = prev_root;

    branch_before_branch_hex = last_branch_hex_seen;
    last_branch_hex_seen = path.Get(idx - 1);
  }

  std::byte current_hex = path.Get(idx);
  switch (root->type) {
  case Node::Type::kBranch:
  {
    Node *next = root->child[std::to_integer<size_t>(current_hex)];
    if (next) {
      idx++;
      branch_hex = current_hex;
      return TraverseToBottom(next, root, last_branch_seen,
                              last_branch_hex_seen, path, idx, branch_hex,
                              existing_key, branch_before_branch,
                              branch_before_branch_hex);
    }
    else {
      // There is no child, so we have reached the bottom of the trie.
      return root;
    }
    break;
  }
  case Node::Type::kExtension:
  {
    if (path.GetSubarray(idx, idx + root->path.size()) == root->path) {
      assert(root->child[0]);
      idx += root->path.size();
      return TraverseToBottom(root->child[0], root, last_branch_seen,
                              last_branch_hex_seen, path, idx, branch_hex,
                              existing_key, branch_before_branch,
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
  case Node::Type::kLeaf:
    if (path.GetSubarray(idx, path.size()) == root->path) {
      existing_key = root;
    }
    // We are going to return the parent node, so subtract 1 from [idx].
    idx--;
    // printf("ret leaf here\n");
    return prev_root;
    break;
  }
}

// static
void
MerkleTrie::GenerateHashesFrom(Node *n)
{
  while (true) {
    if (!n) {
      break;
    }

    n->hash = n->GetNodeHash();
    n = n->parent;
  }
}

// static
void
MerkleTrie::FreeAllNodes(Node *root)
{
  HexArray prefix;
  auto f = [](const HexArray &path, Node &node) { delete &node; };
  Walk(root, prefix, f);
}

} // namespace hogd
