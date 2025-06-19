#include "hogd/container/merkle_trie.h"

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

namespace hogd {

namespace {

size_t
GetLongestCommonRangeLength(const HexArray &a, size_t a_start,
                            const HexArray &b, size_t b_start)
{
  CHECK_LE(a_start, a.size());
  CHECK_LE(b_start, b.size());

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

bool
BranchHasChildHash(const Node::Raw &node, const SHA256Hash &child_hash)
{
  size_t num_matches = 0;
  for (SHA256Hash h : node.child_hashes) {
    if (h == child_hash) {
      num_matches++;
    }
  }
  return num_matches == 1;
}

void
CheckBranch(Node *branch)
{
  if (branch->path.size()) {
    printf("Unexpected path in branch: [%s]\n", branch->path.str().c_str());
  }
  size_t num_children = 0;
  for (Node *child : branch->child) {
    if (child) {
      num_children++;
    }
  }
  if (!num_children) {
    printf("Branch node has no children\n");
  }
  if (!branch->value && num_children == 1) {
    // Check if this is the root node. The root node is a branch that is
    // allowed to have no children.
    if (branch->parent) {
      printf("Branch node has no value and only one child\n");
    }
  }
}

void
CheckExtension(Node *extension)
{
  if (!extension->path.size()) {
    printf("Unexpected empty path in extension: [%s]\n",
           extension->path.str().c_str());
  }
  if (extension->child[0]) {
    if (extension->child[0]->type == Node::Type::kExtension) {
      printf("Extension node has extension node as child\n");
    }
    else if (extension->child[0]->type == Node::Type::kLeaf) {
      printf("Extension node has leaf node as child\n");
    }
  }
  else {
    printf("Extension node has no child\n");
  }
}

void
CheckLeaf(Node *leaf)
{
  if (!leaf->value) {
    printf("Unexpected empty value in leaf\n");
  }
}

} // namespace

// static
std::string
MerkleTrie::StringFromNode(Node *root)
{
  HexArray prefix;
  std::ostringstream oss;
  auto per_node = [&oss](const HexArray &path, Node &node) {
    oss << node << std::endl;
  };
  Walk(root, prefix, per_node);
  oss << "----------" << std::endl;
  return oss.str();
}

std::string
MerkleTrie::str() const
{
  return StringFromNode(root_);
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
    CHECK_PTR(root->child[0]);

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
  const std::pair<SHA256Hash, std::vector<std::vector<std::byte>>> proof =
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
      if (num_nodes) {
        verified_key_size++;
        ok = BranchHasChildHash(node, prev_hash);
      }
      else {
        // We could start on a branch if the value is directly inside the
        // branch.
        ok = (node.value == value);
      }
      break;
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
      CHECK(false);
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
  CHECK_EQ(idx, encodings.size());
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
      break;
    case Node::Type::kLeaf:
    {
      // A leaf must always have a value.
      CHECK(node->value.has_value());
      // Get the [std::string] out of the [std::optional<std::string>] with
      // the dereference operator.
      std::string &value = *node->value;
      return &value;
      break;
    }
    default:
      CHECK(false);
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
  Node *branch =
      TraverseToBottom(root_, /*prev_root=*/nullptr,
                       /*last_branch_seen=*/nullptr,
                       /*last_branch_hex_seen=*/std::byte(0x00),
                       /*path=*/k, idx, branch_hex, existing_key,
                       branch_before_branch, branch_before_branch_hex);
  if (existing_key) {
    // [k] already exists in the trie, so we cannot insert it.
    return false;
  }
  CHECK_PTR(branch);
  CHECK_EQ(branch->type, Node::Type::kBranch);
  CHECK_LE(idx, k.size());

  Node *to_hash = nullptr;
  if (idx == k.size()) {
    // The key maps to a branch node, so we should add the value directly into
    // the branch node.
    CHECK(!branch->value.has_value());
    branch->value = std::move(obj);
    to_hash = branch;
  }
  else {
    // The key does not map to an existing node. We must next check the branch
    // to see if it has any children on the path corresponding to the key. If
    // so, we need to handle the children, such as by splitting part an
    // extension node. If there are no children, we can create a leaf node for
    // the key and add it as a child of the branch.
    size_t branch_idx = std::to_integer<size_t>(k.Get(idx));
    if (branch->child[branch_idx]) {
      // There is already a child node. [TraverseToBottom] returns the lowest
      // branch node in the trie on [key]'s path, so the child node is either an
      // extension node or a leaf node.
      if (branch->child[branch_idx]->type == Node::Type::kExtension) {
        to_hash = InsertHandleExtension(branch->child[branch_idx], branch, k,
                                        idx, std::move(obj));
      }
      else {
        CHECK_EQ(branch->child[branch_idx]->type, Node::Type::kLeaf);
        to_hash = InsertHandleLeaf(branch->child[branch_idx], branch, k, idx,
                                   std::move(obj));
      }
    }
    else {
      // There is no child node, so we create a leaf for [key] and place the
      // leaf as a child of the branch.
      Node *leaf = Node::CreateLeaf(/*parent=*/branch);
      // We are going to use the hex character at [idx] in the key to map to
      // the leaf node from the branch node. Thus, we add 1 below as we do not
      // want to include this character in the path that is stored in the leaf
      // node.
      leaf->path = k.GetSubarray(idx + 1, k.size());
      leaf->value = std::move(obj);
      branch->child[branch_idx] = leaf;
      to_hash = leaf;
    }
  }
  CHECK_PTR(to_hash);

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
  CHECK_PTR(branch);
  CHECK_EQ(branch->type, Node::Type::kBranch);

  Node *to_hash = nullptr;
  if (existing_key == branch) {
    // The key is in a branch.
    CHECK(branch->value.has_value());
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
    CHECK(num_branches);

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
        CHECK_EQ(cur->type, Node::Type::kExtension);
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

      Node *new_child = nullptr;
      if (num_branches == 1) {
        if (other_child) {
          cur_path.Append(other_child_hex);
          new_child = Node::CreateLeaf(/*parent=*/branch_before_branch);
          new_child->path = cur_path;
          new_child->value = std::move(branch->value);
        }
        else {
          // TODO: What if the parent is an extension node?
          if (branch->value.has_value()) {
            // The branch has a value, so the place the value in a leaf with
            // path size 0.
            new_child = Node::CreateLeaf(/*parent=*/branch_before_branch);
            new_child->path = cur_path;
            new_child->value = std::move(branch->value);
          }
        }
        // delete branch;
      }
      else {
        CHECK_PTR(other_child);
        CHECK_EQ(num_branches, 2);

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
      CHECK_EQ(branch, root_);
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
    CheckBranch(root);
    for (Node *child : root->child) {
      CheckHelper(child, root);
    }
    break;
  }
  case Node::Type::kExtension:
    CheckExtension(root);
    CheckHelper(root->child[0], root);
    break;
  case Node::Type::kLeaf:
    CheckLeaf(root);
    break;
  default:
    printf("Unknown node type: %d\n", static_cast<int>(root->type));
    break;
  }
}

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

// We are inserting [k] into the trie, and it conflicts with an extension node
// on the same path. This method resolves that conflict.
//
// Example:
// We are inserting [k = 0x1234] into the trie and it conflicts with an
// extension node with path [0x256]. In this case, we will replace the extension
// node with the following:
//
// First Extension Node: Path is [0x2] with child c. Child is the branch node
//                       below.
// Branch Node: Has a leaf for [k] as child [0x3]. Has the second extension node
//              below as as child [0x5].
// Second Extension Node: Path is [0x6]. The child is c.
Node *
MerkleTrie::InsertHandleExtension(Node *extension, Node *parent_branch,
                                  const Key &k, size_t idx, T &&obj)
{
  CHECK_EQ(extension->type, Node::Type::kExtension);
  CHECK_GT(extension->path.size(), 0);

  // [key_window] holds the portion of [k] that we are currently looking at. In
  // other words, we omit the prefix of [k] that corresponds to the nodes in the
  // tree we have already visited.
  //
  // We add 1 since one of the digits will be included in the [parent_branch]
  // node.
  const Key key_window = k.GetSubarray(idx + 1, k.size());

  size_t common_range_length =
      GetLongestCommonRangeLength(key_window, 0, extension->path, 0);
  size_t diverge_idx = common_range_length;

  // We are going to create a new branch node, with a leaf for [key_window] as
  // one child and the existing subtree as the other child.
  //
  // See the comments below. It is possible that we place [key_window] directly
  // into the branch. It is also possible that we omit part of the existing
  // subtree, too.
  Node *branch = nullptr;
  if (common_range_length) {
    // [key_window] and the extension node have a common range, so we are going
    // to break the extension node into two extension nodes:
    //
    // 1. The first extension node will have the path that is present in both
    // [key_window] and the existing extension node.
    //
    // 2. The second extension node will have the path that is not present in
    // [key_window]. This extension node will be a child of the new branch.
    //
    // Example:
    // If [key_window] has path [0x12ab] and [extension->path] is [0x1234], then
    // the following nodes will be created:
    //
    // First Extension Node: Path is [0x12].
    // Branch Node: A leaf for [key_window] is child [0xa] and the second
    //              extension node is child [0x3].
    // Second Extension Node: Path is [0x4].
    // Leaf Node: Path is [0xb].
    //
    // Note that the second extension node may have a path length of 1, in which
    // case we will omit the second extension node and add its child directly as
    // a child of the new branch.
    // Example:
    // If [key_window] has path [0x1234] and [extension->path] is [0x1235], then
    // the following nodes will be created:
    //
    // First Extension Node: Path is [0x123].
    // Branch Node: A leaf for [key_window] is child [0x4]. The child of
    //              [extension] is child [0x5] in this branch.
    Node *first = Node::CreateExtension(/*parent=*/parent_branch);
    first->path = key_window.GetSubarray(0, diverge_idx);

    // Place the first extension node as a child of the branch.
    parent_branch->child[std::to_integer<size_t>(k.Get(idx))] = first;

    // Create the new branch node and add it as a child of the first extension
    // node.
    branch = Node::CreateBranch(/*parent=*/first);
    first->child[0] = branch;
  }
  else {
    // [key_window] and the extension node do not have a common range. For
    // example, consider the following:
    // Branch Node: The extension node below is child [0x1].
    // Extension Node: Path is [0x234].
    //
    // Any leaves under the extension node have [0x1234] in their path.
    //
    // If [key_window] is [0x1abc], then the nodes for [key_window] will
    // correspond to child [0x1] in the branch.
    //
    // In this case, we create a new branch and place that branch as child [0x1]
    // of the branch node mentioned above. Then we place the extension node as
    // one child of this new branch and we create a leaf node for [key_window]
    // and place the leaf as the other child of the new branch.
    branch = Node::CreateBranch(/*parent=*/parent_branch);
    parent_branch->child[std::to_integer<size_t>(k.Get(idx))] = branch;
  }
  CHECK_PTR(branch);

  // In both cases above, we create a new branch node and now we need to place
  // an extension node as the child of that new branch node.
  //
  // However, note that in the first case above, we may omit the second
  // extension node and instead place the extension node's child directly as a
  // child of the branch.
  CHECK_LT(diverge_idx, extension->path.size());
  std::byte second_branch = extension->path.Get(diverge_idx);
  if (diverge_idx < extension->path.size() - 1) {
    // The second extension node has a path size larger than 1.
    branch->child[std::to_integer<size_t>(second_branch)] = extension;
    extension->parent = branch;

    // Add 1 to account for the branch node. We are placing the second extension
    // node as a child of the branch node, so the branch node accounts for 1 hex
    // digit in the path.
    extension->path =
        extension->path.GetSubarray(diverge_idx + 1, extension->path.size());
    // Note that this second extension node is *not* on the path from the node
    // that will hold [key_window] to the root. Thus, when we rehash the nodes
    // on [key_window]'s path in the trie, we will not rehash this second
    // extension node. Thus, to ensure that the hashes are generated properly in
    // the trie, we need to hash this second extension node here.
    extension->hash = extension->GetNodeHash();
  }
  else {
    // The second extension node would have a path size of 1, so we instead
    // place the second extension node's child directly as a child of the
    // branch.
    branch->child[std::to_integer<size_t>(second_branch)] = extension->child[0];
    extension->child[0]->parent = branch;

    // TODO: Free [extension].
  }

  // Now we need to place [key_window] into the trie. If [key_window] entirely
  // overlaps with the path in the first extension node, then we place
  // [key_window] directly into the new branch. Otherwise, we create a leaf node
  // for [key_window] and add the leaf as a child of the new branch.
  if (diverge_idx >= key_window.size()) {
    // Place [key_window] directly into the branch.
    CHECK_EQ(diverge_idx, key_window.size());
    branch->value = std::move(obj);
    return branch;
  }
  else {
    // Create new leaf node for [key_window] and add the leaf node as a child of
    // the new branch.
    Node *leaf = Node::CreateLeaf(/*parent=*/branch);
    // Add 1 to account for the branch node. Since we are placing the leaf node
    // for [key_window] as a child of the branch, the branch accounts for 1 hex
    // digit in the path.
    leaf->path = key_window.GetSubarray(diverge_idx + 1, key_window.size());
    leaf->value = std::move(obj);

    std::byte leaf_branch = key_window.Get(diverge_idx);
    branch->child[std::to_integer<size_t>(leaf_branch)] = leaf;
    return leaf;
  }
}

// We are inserting [k] into the trie, and it conflicts with a leaf node on the
// same path. This method resolves that conflict.
//
// We create a new branch node and place both [k] and the key in [leaf] as
// children of this branch. Note that if one of the keys ends on the branch, we
// place the key directly into the branch node rather than create a leaf for it.
//
// If [k] and the path in [leaf] have hex digits in common, we also create an
// extension node with the shared hex digits. We place the new branch node as a
// child of this extension node.
Node *
MerkleTrie::InsertHandleLeaf(Node *leaf, Node *parent_branch, const Key &k,
                             size_t idx, T &&obj)
{
  CHECK_EQ(leaf->type, Node::Type::kLeaf);

  // We add 1 since one of the digits will be included in the [parent_branch]
  // node.
  size_t parent_branch_digit = std::to_integer<size_t>(k.Get(idx));
  Key key_window = k.GetSubarray(idx + 1, k.size());

  // We create a branch that we will put both keys under.
  Node *branch = Node::CreateBranch(/*parent=*/parent_branch);
  parent_branch->child[parent_branch_digit] = branch;

  // Check to see if [key_window] and the path in [leaf] have any digits in
  // common. If they do, we create an extension node with the shared digits, and
  // then we place the new branch as a child of this new extension node.
  size_t common_range_length =
      GetLongestCommonRangeLength(key_window, 0, leaf->path, 0);
  if (common_range_length) {
    size_t diverge_idx = common_range_length;
    Node *extension = Node::CreateExtension(/*parent=*/parent_branch);
    extension->path = key_window.GetSubarray(0, diverge_idx);

    parent_branch->child[parent_branch_digit] = extension;
    extension->child[0] = branch;
    branch->parent = extension;

    // Now remove the extension path from these keys.
    key_window = key_window.GetSubarray(diverge_idx, key_window.size());
    leaf->path = leaf->path.GetSubarray(diverge_idx, leaf->path.size());
  }

  // Now we place both keys in the branch. Note that it is possible one of the
  // keys is shorter than the other. If so, it is possible the shorter key's
  // remaining path is empty, in which case it should be placed directly into
  // the branch rather than in a leaf node that is a child of the branch.
  const bool key_shorter = key_window.size() <= leaf->path.size();
  const Key &shorter_path = key_shorter ? key_window : leaf->path;
  const Key &longer_path = key_shorter ? leaf->path : key_window;
  const std::string &shorter_value = key_shorter ? obj : *leaf->value;
  const std::string &longer_value = key_shorter ? *leaf->value : obj;

  if (shorter_path.empty()) {
    // The shorter key's remaining path is empty, so we should place the shorter
    // key directly into the branch.
    branch->value = std::move(shorter_value);
  }
  else {
    CHECK(!longer_path.empty());
    CHECK_NE(static_cast<uint8_t>(shorter_path.Get(0)),
             static_cast<uint8_t>(longer_path.Get(0)));
    // We place the two keys into their own leaf nodes and we make those leaf
    // nodes children of the branch.

    // Shorter key's leaf node. Note that we reuse [leaf] for the longer node
    // below, so we allocate a new node here.
    //
    // Note that I am deliberately allocating a new node first *before* reusing
    // [leaf]. The [shorter_*] and [longer_*] variables above are references to
    // elements in [leaf], so we cannot overwrite [leaf] until we no longer need
    // those references.
    Node *shorter_leaf = Node::CreateLeaf(/*parent=*/branch);
    // Unlike the other nodes, we do not need to call [shorter_path.Get(0)]
    // before writing [shorter_leaf->path] because we are not reusing [leaf].
    // Thus, [shorter_path] will never be a reference to [shorter_leaf->path]. I
    // still call it first though to match the style of the node creation below.
    branch->child[std::to_integer<size_t>(shorter_path.Get(0))] = shorter_leaf;
    shorter_leaf->path = shorter_path.GetSubarray(1, shorter_path.size());
    shorter_leaf->value = std::move(shorter_value);
    // We hash this leaf here, and then in the parent function, we hash all
    // nodes on the path from the leaf *created below* to the root. Note that
    // this leaf is not on that path, so we must hash it here.
    shorter_leaf->hash = shorter_leaf->GetNodeHash();
  }

  // Now we place the longer key into a leaf node that is a child of the branch.
  // Note that we reuse [leaf] rather than allocate a new node.
  //
  // We overwrite [leaf->path] below, so we need call [longer_path.Get(0)] first
  // as [longer_path] may be a reference to [leaf->path].
  branch->child[std::to_integer<size_t>(longer_path.Get(0))] = leaf;
  leaf->path = longer_path.GetSubarray(1, longer_path.size());
  leaf->value = std::move(longer_value);
  leaf->parent = branch;

  return leaf;
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
      if (!root->value->empty()) {
        existing_key = root;
      }
      return root;
    }
    else if (prev_root && prev_root->type == Node::Type::kBranch) {
      if (root->type == Node::Type::kLeaf && root->path.size() == 0) {
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
    if (idx + root->path.size() <= path.size() &&
        path.GetSubarray(idx, idx + root->path.size()) == root->path) {
      CHECK_PTR(root->child[0]);
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
