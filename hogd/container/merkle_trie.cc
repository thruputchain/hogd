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

bool
BranchGetChildIndex(const Node &branch, Node *child, size_t &idx)
{
  for (idx = 0; idx < branch.child.size(); idx++) {
    if (branch.child[idx] == child) {
      return true;
    }
  }
  return false;
}

size_t
GetNumChildren(const Node &node)
{
  size_t num_children = 0;
  for (const Node *child : node.child) {
    if (child) {
      num_children++;
    }
  }
  return num_children;
}

bool
CheckBranch(Node *branch)
{
  bool ret = true;
  if (branch->path.size()) {
    printf("Unexpected path in branch: [%s]\n", branch->path.str().c_str());
    ret = false;
  }
  size_t num_children = GetNumChildren(*branch);
  bool is_root = !branch->parent;
  if (!num_children && !is_root) {
    printf("Branch node has no children\n");
    ret = false;
  }
  if (!branch->value && num_children == 1) {
    // Check if this is the root node. The root node is a branch that is
    // allowed to have no children.
    if (!is_root) {
      printf("Branch node has no value and only one child\n");
      ret = false;
    }
  }
  if (!is_root) {
    if (branch->parent->type != Node::Type::kBranch &&
        branch->parent->type != Node::Type::kExtension) {
      printf("Branch node has unexpected parent node type: %s\n",
             Node::GetTypeString(branch->parent->type).c_str());
      ret = false;
    }
  }
  return ret;
}

bool
CheckExtension(Node *extension)
{
  bool ret = true;
  if (extension->value) {
    printf("Unexpected value in extension\n");
    ret = false;
  }
  size_t num_children = GetNumChildren(*extension);
  if (num_children > 1) {
    printf("Unexpected multiple children in extension\n");
    ret = false;
  }
  if (!extension->path.size()) {
    printf("Unexpected empty path in extension: [%s]\n",
           extension->path.str().c_str());
    ret = false;
  }
  if (extension->child[0]) {
    if (extension->child[0]->type == Node::Type::kExtension) {
      printf("Extension node has extension node as child\n");
      ret = false;
    }
    else if (extension->child[0]->type == Node::Type::kLeaf) {
      printf("Extension node has leaf node as child\n");
      ret = false;
    }
  }
  else {
    printf("Extension node has no child\n");
    ret = false;
  }
  if (extension->parent->type != Node::Type::kBranch) {
    printf("Extension node has unexpected parent node type: %s\n",
           Node::GetTypeString(extension->parent->type).c_str());
    ret = false;
  }
  return ret;
}

bool
CheckLeaf(Node *leaf)
{
  bool ret = true;
  size_t num_children = GetNumChildren(*leaf);
  if (num_children) {
    printf("Unexpected children in leaf\n");
    ret = false;
  }
  // A path in the leaf node may or may not be empty, so we do not check its
  // size.
  if (!leaf->value) {
    printf("Unexpected empty value in leaf\n");
    ret = false;
  }
  if (leaf->parent->type != Node::Type::kBranch) {
    printf("Extension Leaf has unexpected parent node type: %s\n",
           Node::GetTypeString(leaf->parent->type).c_str());
    ret = false;
  }
  return ret;
}

} // namespace

// static
std::string
MerkleTrie::StringFromNode(const Node *root)
{
  HexArray prefix;
  std::ostringstream oss;
  auto per_node = [&oss](const HexArray &path, const Node &node) {
    oss << node << std::endl;
  };
  Walk<const Node>(root, prefix, per_node);
  oss << "----------" << std::endl;
  return oss.str();
}

std::string
MerkleTrie::str() const
{
  return StringFromNode(&root_);
}

// static
template<typename NodeType>
void
MerkleTrie::Walk(NodeType *root, HexArray &prefix,
                 const std::function<void(const HexArray &, NodeType &)> &f)
{
  static_assert(std::is_same_v<NodeType, Node> ||
                    std::is_same_v<NodeType, const Node>,
                "[NodeType] must be [Node] or [const Node].");

  if (!root) {
    return;
  }

  switch (root->type) {
  case Node::Type::kBranch:
    for (size_t i = 0; i < root->child.size(); i++) {
      std::byte digit = static_cast<std::byte>(i);

      prefix.Append(digit);
      Walk<NodeType>(root->child[i], prefix, f);
      prefix.RemoveFromBack(1);
    }
    f(prefix, *root);
    break;
  case Node::Type::kExtension:
    CHECK_PTR(root->child[0]);

    prefix.Append(root->path);
    Walk<NodeType>(root->child[0], prefix, f);
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
  // Combine all of the node encodings into a single vector of bytes.
  for (const std::vector<std::byte> &node_encoding : proof.second) {
    ret.second.insert(ret.second.end(), node_encoding.begin(),
                      node_encoding.end());
  }
  return ret;
}

// static
bool
MerkleTrie::Prove(const SHA256Hash &root_hash,
                  const std::vector<std::byte> &encodings, const Key &k,
                  const T &value)
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

  // Check that we verified the entire key and that the root hash matches
  // up.
  return verified_key_size == k.size() && node.hash == root_hash;
}

MerkleTrie::T *
MerkleTrie::Find(const Key &k)
{
  Node *node = FindNode(k);
  if (!node) {
    return nullptr;
  }

  switch (node->type) {
  case Node::Type::kBranch:
    // Check if the branch has a value.
    if (node->value) {
      // Get the [T] out of the [std::optional<T>] with the dereference
      // operator.
      T &value = *node->value;
      return &value;
    }
    return nullptr;
    break;
  case Node::Type::kLeaf:
  {
    // A leaf must always have a value.
    CHECK(node->value.has_value());
    // Get the [T] out of the [std::optional<T>] with the dereference operator.
    T &value = *node->value;
    return &value;
    break;
  }
  default:
    CHECK(false);
    return nullptr;
    break;
  }
}

bool
MerkleTrie::Insert(const Key &k, T &&obj)
{
  size_t idx = 0;
  Node *existing_key = nullptr;
  Node *branch =
      TraverseToLowestBranch(/*path=*/k, &root_, /*prev_root=*/nullptr, idx,
                             existing_key);
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
      // There is already a child node. [TraverseToLowestBranch] returns the
      // lowest branch node in the trie on [key]'s path, so the child node is
      // either an extension node or a leaf node.
      if (branch->child[branch_idx]->type == Node::Type::kExtension) {
        to_hash = InsertHandleExtension(branch->child[branch_idx], k, idx,
                                        std::move(obj));
      }
      else {
        CHECK_EQ(branch->child[branch_idx]->type, Node::Type::kLeaf);
        to_hash =
            InsertHandleLeaf(branch->child[branch_idx], k, idx, std::move(obj));
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

// Removes [branch] because it only has one KV pair "under" it, so it is no
// longer needed.
//
// static
Node *
MerkleTrie::RemoveBranch(const Key &k, Node *branch, size_t num_children)
{
  // We should only remove a branch if it has one KV pair "under" it.
  if (num_children) {
    CHECK_EQ(num_children, 1);
  }
  else {
    CHECK(branch->value.has_value());
  }
  // We should not remove the root node.
  CHECK_PTR(branch->parent);

  // It is possible that the branch's parent is an extension node. In this case,
  // we need to remove the extension node, too, and then we need to add the
  // extension node's path as a prefix to the child in the branch. We use
  // [cur_path] to hold the extension node's path (if needed).
  HexArray cur_path;

  // This is the hex digit in the parent branch that corresponds to the path
  // that [k] is on. We are removing [branch] from the trie, so we need to know
  // this hex digit so that we know which of the parent branch's child pointers
  // to update.
  size_t parent_branch_hex;

  // Check if the branch's parent is an extension node.
  if (branch->parent->type == Node::Type::kExtension) {
    Node *extension = branch->parent;
    // [extension] is the child of the parent branch, so we use it to set
    // [parent_branch_hex].
    CHECK(
        BranchGetChildIndex(*extension->parent, extension, parent_branch_hex));
    cur_path.Append(branch->parent->path);

    // Remove the extension node as a child of the parent branch. Replace it
    // with [branch].
    extension->parent->child[parent_branch_hex] = branch;
    branch->parent = extension->parent;

    delete extension;
  }
  else {
    CHECK_EQ(branch->parent->type, Node::Type::kBranch);
    // The branch's parent is the parent branch itself. Thus, we use [branch] to
    // set [parent_branch_hex].
    CHECK(BranchGetChildIndex(*branch->parent, branch, parent_branch_hex));
  }
  CHECK_EQ(branch->parent->type, Node::Type::kBranch);

  // Get the node that is the child of [branch], if one exists.
  Node *child = nullptr;
  std::byte child_hex;
  for (size_t i = 0; i < branch->child.size(); i++) {
    Node *c = branch->child[i];
    if (c) {
      child = c;
      child_hex = static_cast<std::byte>(i);
      break;
    }
  }

  Node *new_child = nullptr;
  // We are going to reuse or free [branch] in the if/else bodies below, so we
  // cache [branch->parent] since we need it at the bottom of the function.
  Node *const branch_parent = branch->parent;
  if (num_children == 0) {
    CHECK_EQ(child, nullptr);
    CHECK(branch->value.has_value());

    // The branch has a value, so the place the value in a leaf with path size
    // 0.
    //
    // We reuse the branch node below.
    std::optional<T> value = std::move(branch->value);
    Node *parent = branch->parent;
    new_child = branch;
    new_child->Clear();
    new_child->InitLeaf(parent);
    new_child->path = cur_path;
    new_child->value = std::move(value);
  }
  else {
    CHECK_PTR(child);
    CHECK_EQ(num_children, 1);
    CHECK(!branch->value.has_value());

    if (child->type == Node::Type::kBranch) {
      // The remaining child under [branch] is also a branch. Thus, we can
      // replace [branch] with an extension node. The path in the extension node
      // will be [cur_path] (to account for a possible extension node we removed
      // above) followed by the hex digit in [branch] that the remaining child
      // is under.
      cur_path.Append(child_hex);
      CHECK_GE(cur_path.size(), 1);
      // We reuse the branch node below.
      new_child = branch;
      Node *parent = branch->parent;
      new_child->Clear();
      new_child->InitExtension(parent);
      new_child->path = cur_path;
      new_child->child[0] = child;
      child->parent = new_child;
    }
    else {
      // The remaining child under [branch] is either an extension node or a
      // leaf node. Either way, we will add a prefix to the path in the node.
      // The prefix will be [cur_path] (to account for a possible extension node
      // we removed above) followed by the hex digit in the branch that the
      // remaining child is under.
      cur_path.Append(child_hex);

      // Add [child->path] to [cur_path] since we then overwrite [child->path]
      // entirely with [cur_path]. We want to make sure the existing path in
      // [child] remains.
      cur_path.Append(child->path);
      child->path = cur_path;

      new_child = child;
      delete branch;
    }
  }

  // Now add [new_child] as a child of [parent_branch].
  CHECK_PTR(new_child);
  branch_parent->child[parent_branch_hex] = new_child;
  new_child->parent = branch_parent;

  return new_child;
}

bool
MerkleTrie::Remove(const Key &k)
{
  size_t idx = 0;
  Node *existing_key = nullptr;
  Node *branch =
      TraverseToLowestBranch(/*path=*/k, &root_, /*prev_root=*/nullptr, idx,
                             existing_key);
  if (!existing_key) {
    // [k] does not exist in the trie, so we cannot remove it.
    return false;
  }
  CHECK_PTR(branch);
  CHECK_EQ(branch->type, Node::Type::kBranch);

  size_t num_children = GetNumChildren(*branch);
  if (existing_key == branch) {
    // [k] maps to a branch, so clear the branch's value.
    CHECK(branch->value.has_value());
    branch->value = std::nullopt;
  }
  else {
    // [existing_key != branch], so we know that the key is leaf node which is a
    // child of a branch. Thus, we need to remove this leaf node.
    CHECK_EQ(existing_key->type, Node::Type::kLeaf);

    size_t branch_hex = std::to_integer<size_t>(k.Get(idx));
    CHECK_EQ(branch->child[branch_hex], existing_key);
    CHECK(num_children);

    // Remove [k]'s leaf as a child of the branch.
    branch->child[branch_hex] = nullptr;
    num_children--;
    delete existing_key;
  }

  Node *to_hash = nullptr;
  if (branch == &root_) {
    // If the branch is the root node, we need to keep it. Even if the branch
    // now has fewer than two KV pairs "under" it, there is no further
    // optimization possible.
    to_hash = branch;
  }
  else if (num_children > 1 || (num_children == 1 && branch->value)) {
    // The branch has multiple children, or there is exactly one child and there
    // is a value in the branch (so in other words, there are two KV pairs
    // "under" this branch). Thus, as the branch still has at least two KV pairs
    // "under" it, no further optimization is possible.
    to_hash = branch;
  }
  else {
    // The branch now has fewer than two KV pairs "under" it, so we can
    // optimize our trie by removing the branch.
    to_hash = RemoveBranch(k, branch, num_children);
  }

  size_--;
  CHECK_PTR(to_hash);
  GenerateHashesFrom(to_hash);
  return true;
}

// static
bool
MerkleTrie::CheckHelper(Node *root, Node *parent)
{
  if (!root) {
    return true;
  }

  bool ret = true;
  if (root->parent != parent) {
    printf("The node's parent pointer does not match its actual parent.\n");
    ret = false;
  }

  if (root->hash != root->ComputeNodeHash()) {
    printf(
        "The hash in the node does not match the hash we just calculated.\n");
    ret = false;
  }
  switch (root->type) {
  case Node::Type::kBranch:
  {
    // The functions below print debug information, so we always want the
    // functions to be called.
    //
    // Thus, we deliberately include [ret] second in the AND statements below to
    // ensure that the functions in the first part of the AND are always called,
    // even when [ret] is already [false].
    ret = CheckBranch(root) && ret;
    for (Node *child : root->child) {
      ret = CheckHelper(child, root) && ret;
    }
    break;
  }
  case Node::Type::kExtension:
    ret = CheckExtension(root) && ret;
    ret = CheckHelper(root->child[0], root) && ret;
    break;
  case Node::Type::kLeaf:
    ret = CheckLeaf(root) && ret;
    break;
  default:
    ret = false;
    printf("Unknown node type: %d\n", static_cast<int>(root->type));
    break;
  }
  return ret;
}

Node *
MerkleTrie::FindNode(const Key &k)
{
  size_t idx = 0;
  Node *existing_key = nullptr;
  TraverseToLowestBranch(/*path=*/k, &root_, /*prev_root=*/nullptr, idx,
                         existing_key);
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
//
// static
Node *
MerkleTrie::InsertHandleExtension(Node *extension, const Key &k, size_t idx,
                                  T &&obj)
{
  CHECK_EQ(extension->type, Node::Type::kExtension);
  CHECK_EQ(extension->parent->type, Node::Type::kBranch);
  CHECK_GT(extension->path.size(), 0);

  // [key_window] holds the portion of [k] that we are currently looking at. In
  // other words, we omit the prefix of [k] that corresponds to the nodes in the
  // trie we have already visited.
  //
  // We add 1 since one of the digits will be included in the
  // [extension->parent] node, which is always a branch node.
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
  // We are going to reuse [extension] in the if/else bodies below, so we cache
  // the node since we need its contents after it is reused.
  const Node extension_cache = *extension;
  size_t parent_branch_hex = std::to_integer<size_t>(k.Get(idx));
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
    Node *first = Node::CreateExtension(/*parent=*/extension_cache.parent);
    first->path = key_window.GetSubarray(0, diverge_idx);

    // Place the first extension node as a child of the branch.
    extension_cache.parent->child[parent_branch_hex] = first;

    // Create the new branch node and add it as a child of the first extension
    // node.
    //
    // Note that we reuse [extension] below.
    branch = extension;
    branch->Clear();
    branch->InitBranch(/*parent=*/first);
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
    //
    // Note that we reuse [extension] below.
    branch = extension;
    branch->Clear();
    branch->InitBranch(/*parent=*/extension_cache.parent);
    // We are reusing [extension] as [branch], so they are the same pointer.
    // Thus, we can technically omit the line below below, but we keep it for
    // clarity.
    extension_cache.parent->child[parent_branch_hex] = branch;
  }
  CHECK_PTR(branch);
  // Set [extension] to [nullptr] so that we crash if we accidentally use it
  // below without reassigning it.
  extension = nullptr;

  // In both cases above, we create a new branch node and now we need to place
  // an extension node as the child of that new branch node.
  //
  // However, note that in the first case above, we may omit the second
  // extension node and instead place the extension node's child directly as a
  // child of the branch.
  CHECK_LT(diverge_idx, extension_cache.path.size());
  size_t second_branch =
      std::to_integer<size_t>(extension_cache.path.Get(diverge_idx));
  if (diverge_idx < extension_cache.path.size() - 1) {
    extension = Node::CreateExtension(/*parent=*/branch);
    // The second extension node has a path size larger than 1.
    branch->child[second_branch] = extension;

    // Add 1 to account for the branch node. We are placing the second extension
    // node as a child of the branch node, so the branch node accounts for 1 hex
    // digit in the path.
    extension->path =
        extension_cache.path.GetSubarray(diverge_idx + 1,
                                         extension_cache.path.size());
    extension->child[0] = extension_cache.child[0];
    // [extension] was reused above, so [extension_cache.child[0]->parent]
    // currently points to the reused node rather than the [extension] we just
    // allocated. Thus, we update the parent pointer to [extension].
    extension_cache.child[0]->parent = extension;
    // Note that this second extension node is *not* on the path from the node
    // that will hold [key_window] to the root. Thus, when we rehash the nodes
    // on [key_window]'s path in the trie, we will not rehash this second
    // extension node. Thus, to ensure that the hashes are generated properly in
    // the trie, we need to hash this second extension node here.
    extension->hash = extension->ComputeNodeHash();
  }
  else {
    // The second extension node would have a path size of 1, so we instead
    // place the second extension node's child directly as a child of the
    // branch.
    branch->child[second_branch] = extension_cache.child[0];
    extension_cache.child[0]->parent = branch;
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

    size_t leaf_branch = std::to_integer<size_t>(key_window.Get(diverge_idx));
    branch->child[leaf_branch] = leaf;
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
//
// static
Node *
MerkleTrie::InsertHandleLeaf(Node *leaf, const Key &k, size_t idx, T &&obj)
{
  CHECK_EQ(leaf->type, Node::Type::kLeaf);
  CHECK_EQ(leaf->parent->type, Node::Type::kBranch);

  // We add 1 since one of the digits will be included in the [leaf->parent]
  // node.
  size_t parent_branch_digit = std::to_integer<size_t>(k.Get(idx));
  Key key_window = k.GetSubarray(idx + 1, k.size());

  // We create a branch that we will put both keys under.
  Node *branch = Node::CreateBranch(/*parent=*/leaf->parent);
  leaf->parent->child[parent_branch_digit] = branch;

  // Check to see if [key_window] and the path in [leaf] have any digits in
  // common. If they do, we create an extension node with the shared digits, and
  // then we place the new branch as a child of this new extension node.
  size_t common_range_length =
      GetLongestCommonRangeLength(key_window, 0, leaf->path, 0);
  if (common_range_length) {
    size_t diverge_idx = common_range_length;
    Node *extension = Node::CreateExtension(/*parent=*/leaf->parent);
    extension->path = key_window.GetSubarray(0, diverge_idx);

    leaf->parent->child[parent_branch_digit] = extension;
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
  T &&shorter_value = key_shorter ? obj : std::move(*leaf->value);
  T &&longer_value = key_shorter ? std::move(*leaf->value) : obj;

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
    size_t branch_hex = std::to_integer<size_t>(shorter_path.Get(0));
    branch->child[branch_hex] = shorter_leaf;
    shorter_leaf->path = shorter_path.GetSubarray(1, shorter_path.size());
    shorter_leaf->value = std::move(shorter_value);
    // We hash this leaf here, and then in the parent function, we hash all
    // nodes on the path from the leaf *created below* to the root. Note that
    // this leaf is not on that path, so we must hash it here.
    shorter_leaf->hash = shorter_leaf->ComputeNodeHash();
  }

  // Now we place the longer key into a leaf node that is a child of the branch.
  // Note that we reuse [leaf] rather than allocate a new node.
  //
  // We overwrite [leaf->path] below, so we need call [longer_path.Get(0)] first
  // as [longer_path] may be a reference to [leaf->path].
  size_t branch_hex = std::to_integer<size_t>(longer_path.Get(0));
  branch->child[branch_hex] = leaf;
  leaf->path = longer_path.GetSubarray(1, longer_path.size());
  leaf->value = std::move(longer_value);
  leaf->parent = branch;

  return leaf;
}

// static
Node *
MerkleTrie::TraverseToLowestBranch(const HexArray &path, Node *root,
                                   Node *prev_root, size_t &idx,
                                   Node *&existing_key)
{
  CHECK_PTR(root);
  CHECK_LE(idx, path.size());

  switch (root->type) {
  case Node::Type::kBranch:
  {
    if (idx == path.size()) {
      if (root->value) {
        // [path] maps to this branch node.
        existing_key = root;
      }
      return root;
    }

    size_t branch_hex = std::to_integer<size_t>(path.Get(idx));
    Node *next = root->child[branch_hex];
    if (next) {
      // There is a child that corresponds to the next hex digit in [path], so
      // continue searching. We add 1 to [idx] since the branch corresponds to
      // one hex digit in the path.
      idx++;
      return TraverseToLowestBranch(path, next, root, idx, existing_key);
    }
    else {
      // There is no child, so we have reached the bottom of the trie.
      return root;
    }
    break;
  }
  case Node::Type::kExtension:
  {
    // We check [idx + root->path.size() <= path.size()]. Note that when these
    // two values are equal, the path maps to the child node, which is always a
    // branch.
    if (idx + root->path.size() <= path.size() &&
        path.GetSubarray(idx, idx + root->path.size()) == root->path) {
      idx += root->path.size();
      CHECK_PTR(root->child[0]);
      return TraverseToLowestBranch(path, root->child[0], root, idx,
                                    existing_key);
    }
    else {
      // The extension node's path does not match [key], so we can stop moving
      // down the trie.
      //
      // We are going to return the parent node, which is always a branch, so
      // subtract 1 from [idx] since the branch corresponds to one hex digit in
      // the path.
      idx--;
      return prev_root;
    }
    break;
  }
  case Node::Type::kLeaf:
    if (path.GetSubarray(idx, path.size()) == root->path) {
      // [path] maps to this leaf node.
      existing_key = root;
    }
    // We are going to return the parent node, which is always a branch, so
    // subtract 1 from [idx] since the branch corresponds to one hex digit in
    // the path.
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

    n->hash = n->ComputeNodeHash();
    n = n->parent;
  }
}

void
MerkleTrie::FreeAllNodes(Node *root)
{
  HexArray prefix;
  auto f = [&](const HexArray &path, Node &node) {
    // [root_] is a [MerkleTrie] class member of type [Node]. It is not a
    // pointer to a [Node] value on the heap, so we cannot free the root node.
    if (&node != &root_) {
      delete &node;
    }
  };
  Walk<Node>(root, prefix, f);
}

} // namespace hogd
