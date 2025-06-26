#include "hogd/container/merkle/merkle_trie.h"

#include <cstddef>
#include <cstring>

#include <functional>
#include <optional>
#include <print>
#include <sstream>
#include <stack>
#include <string>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/node.h"
#include "types.hh"

namespace hogd {

namespace {

size_t
get_longest_common_range_length(const HexArray &a, size_t a_start,
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
    if (a.get(i + a_start) != b.get(i + b_start)) {
      break;
    }
    length++;
  }
  return length;
}

bool
branch_has_child_hash(const Node::Raw &node, const SHA256Hash &child_hash)
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
branch_get_child_index(const Node &branch, Node *child, size_t &idx)
{
  for (idx = 0; idx < branch.child.size(); idx++) {
    if (branch.child[idx] == child) {
      return true;
    }
  }
  return false;
}

size_t
get_num_children(const Node &node)
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
check_branch(const Node &branch)
{
  bool ret = true;
  if (!branch.path.empty()) {
    std::println("Unexpected path in branch: [{}]", branch.path.str());
    ret = false;
  }
  size_t num_children = get_num_children(branch);
  bool is_root = branch.parent == nullptr;
  if (!num_children && !is_root) {
    std::println("Branch node has no children");
    ret = false;
  }
  if (!branch.value && num_children == 1) {
    // Check if this is the root node. The root node is a branch that is
    // allowed to have no children.
    if (!is_root) {
      std::println("Branch node has no value and only one child");
      ret = false;
    }
  }
  if (!is_root) {
    if (branch.parent->type != Node::Type::kBranch &&
        branch.parent->type != Node::Type::kExtension) {
      std::println("Branch node has unexpected parent node type: {}",
                   Node::get_type_string(branch.parent->type));
      ret = false;
    }
  }
  return ret;
}

bool
check_extension(const Node &extension)
{
  bool ret = true;
  if (extension.value) {
    std::println("Unexpected value in extension");
    ret = false;
  }
  size_t num_children = get_num_children(extension);
  if (num_children > 1) {
    std::println("Unexpected multiple children in extension");
    ret = false;
  }
  if (extension.path.empty()) {
    std::println("Unexpected empty path in extension: [{}]",
                 extension.path.str());
    ret = false;
  }
  if (extension.child[0]) {
    if (extension.child[0]->type == Node::Type::kExtension) {
      std::println("Extension node has extension node as child");
      ret = false;
    }
    else if (extension.child[0]->type == Node::Type::kLeaf) {
      std::println("Extension node has leaf node as child");
      ret = false;
    }
  }
  else {
    std::println("Extension node has no child");
    ret = false;
  }
  if (extension.parent->type != Node::Type::kBranch) {
    std::println("Extension node has unexpected parent node type: {}",
                 Node::get_type_string(extension.parent->type));
    ret = false;
  }
  return ret;
}

bool
check_leaf(const Node &leaf)
{
  bool ret = true;
  size_t num_children = get_num_children(leaf);
  if (num_children) {
    std::println("Unexpected children in leaf");
    ret = false;
  }
  // A path in the leaf node may or may not be empty, so we do not check its
  // size.
  if (!leaf.value) {
    std::println("Unexpected empty value in leaf");
    ret = false;
  }
  if (leaf.parent->type != Node::Type::kBranch) {
    std::println("Extension Leaf has unexpected parent node type: {}",
                 Node::get_type_string(leaf.parent->type));
    ret = false;
  }
  return ret;
}

} // namespace

// static
std::string
MerkleTrie::string_from_node(const Node &node)
{
  HexArray prefix;
  std::ostringstream oss;
  auto f = [&oss](const HexArray & /*path*/, const Node &node,
                  const Node * /*parent*/) { oss << node << '\n'; };
  walk<const Node>(&node, prefix, f);
  oss << "----------" << '\n';
  return oss.str();
}

// static
bool
MerkleTrie::check_helper(Node *root)
{

  HexArray prefix;
  bool ret = true;
  auto f = [&](const HexArray & /*path*/, const Node &node,
               const Node *parent) {
    if (node.rw_lock.is_locked_relaxed()) {
      std::println("The node is locked.");
      ret = false;
    }
    if (node.parent != parent) {
      std::println(
          "The node's parent pointer does not match its actual parent.");
      ret = false;
    }
    if (node.hash != node.compute_node_hash()) {
      std::println("The hash in the node does not match the hash we just "
                   "calculated.");
      ret = false;
    }

    switch (node.type) {
    case Node::Type::kBranch:
      ret = check_branch(node) && ret;
      break;
    case Node::Type::kExtension:
      ret = check_extension(node) && ret;
      break;
    case Node::Type::kLeaf:
      ret = check_leaf(node) && ret;
      break;
    }
  };
  walk<const Node>(root, prefix, f);

  return ret;
}

std::string
MerkleTrie::str() const
{
  return string_from_node(root_);
}

std::pair<SHA256Hash, std::vector<std::byte>>
MerkleTrie::get_proof(const Key &k)
{
  const std::pair<SHA256Hash, std::vector<std::vector<std::byte>>> &proof =
      get_proof_by_node(k);
  std::pair<SHA256Hash, std::vector<std::byte>> ret;
  ret.first = proof.first;
  // Combine all of the node encodings into a single vector of bytes.
  for (const std::vector<std::byte> &node_encoding : proof.second) {
    ret.second.insert(ret.second.end(), node_encoding.begin(),
                      node_encoding.end());
  }
  return ret;
}

std::pair<SHA256Hash, std::vector<std::vector<std::byte>>>
MerkleTrie::get_proof_by_node(const Key &k)
{
  Node *node = find_node(k);
  if (!node) {
    return {SHA256Hash(), {}};
  }

  std::vector<std::vector<std::byte>> ret;
  while (node) {
    const std::vector<std::byte> &encoding = node->get_node_encoding();
    ret.push_back(encoding);

    node->rw_lock.unlock();
    node = node->parent;
  }
  return {root_.hash, ret};
}

// static
bool
MerkleTrie::prove(const SHA256Hash &root_hash,
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
    if (!Node::get_node_from_encoding(encodings, idx, node)) {
      return false;
    }

    bool ok;
    switch (node.type) {
    case Node::Type::kBranch:
      if (num_nodes) {
        verified_key_size++;
        ok = branch_has_child_hash(node, prev_hash);
      }
      else {
        // We could start on a branch if the value is directly inside the
        // branch.
        ok = (node.value == value);
      }
      break;
    case Node::Type::kExtension:
      ok = (num_nodes != 0U);
      verified_key_size += node.path.size();
      ok = ok && (node.child_hashes[0] == prev_hash);
      break;
    case Node::Type::kLeaf:
      ok = (num_nodes == 0U);
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
MerkleTrie::find(const Key &k)
{
  Node *node = find_node(k);
  if (!node) {
    return nullptr;
  }

  T *ret = nullptr;
  switch (node->type) {
  case Node::Type::kBranch:
    // Check if the branch has a value.
    if (node->value) {
      // Get the [T] out of the [std::optional<T>] with the dereference
      // operator.
      T &value = *node->value;
      ret = &value;
    }
    break;
  case Node::Type::kLeaf:
  {
    // A leaf must always have a value.
    CHECK(node->value.has_value());
    // Get the [T] out of the [std::optional<T>] with the dereference
    // operator.
    T &value = *node->value; // NOLINT(bugprone-unchecked-optional-access)
    ret = &value;
    break;
  }
  default:
    CHECK(false);
    break;
  }

  // TODO(jhumphri): We should hold the writer lock on [node] until the caller
  // indicates they are done accessing the value.
  node->rw_lock.unlock();
  if (node->type == Node::Type::kLeaf) {
    CHECK_PTR(node->parent);
    node->parent->rw_lock.unlock();
  }
  else {
    CHECK_EQ(node->type, Node::Type::kBranch);
  }
  return ret;
}

Node *
MerkleTrie::find_node(const Key &k)
{
  size_t idx = 0;
  Node *existing_key = nullptr;
  Node *branch =
      traverse_to_lowest_branch(/*path=*/k, &root_, idx, existing_key);
  if (!existing_key) {
    branch->rw_lock.unlock();
  }
  return existing_key;
}

bool
MerkleTrie::insert(const Key &k, T &&obj)
{
  size_t idx;
  Node *existing_key;
  Node *branch;
  while (true) {
    idx = 0;
    existing_key = nullptr;

    branch = traverse_to_lowest_branch(/*path=*/k, &root_, idx, existing_key);
    if (existing_key) {
      // [k] already exists in the trie, so we cannot insert it.
      existing_key->rw_lock.unlock();
      if (branch != existing_key) {
        branch->rw_lock.unlock();
      }
      return false;
    }
    if (branch->rw_lock.try_upgrade_to_writer_if_only_one()) {
      break;
    }
    // We need to try again. We cannot block on acquiring the writer lock
    // because we could deadlock with another thread trying to do the same.
    //
    // We also cannot drop the lock and then acquire it as the writer because it
    // is possible that another thread is attempting to insert [k], acquires the
    // writer lock for [branch] before we do, and then inserts [k].
    branch->rw_lock.unlock();
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
    auto branch_idx = std::to_integer<size_t>(k.get(idx));
    if (branch->child[branch_idx]) {
      // There is already a child node. [TraverseToLowestBranch] returns the
      // lowest branch node in the trie on [key]'s path, so the child node is
      // either an extension node or a leaf node.
      if (branch->child[branch_idx]->type == Node::Type::kExtension) {
        to_hash = insert_handle_extension(branch->child[branch_idx], k, idx,
                                          std::move(obj));
      }
      else {
        CHECK_EQ(branch->child[branch_idx]->type, Node::Type::kLeaf);
        to_hash = insert_handle_leaf(branch->child[branch_idx], k, idx,
                                     std::move(obj));
      }
    }
    else {
      // There is no child node, so we create a leaf for [key] and place the
      // leaf as a child of the branch.
      Node *leaf = Node::create_leaf(/*parent=*/branch);
      CHECK(leaf->rw_lock.try_lock_writer());
      // We are going to use the hex character at [idx] in the key to map to
      // the leaf node from the branch node. Thus, we add 1 below as we do not
      // want to include this character in the path that is stored in the leaf
      // node.
      leaf->path = k.get_subarray(idx + 1, k.size());
      leaf->value = std::move(obj);
      branch->child[branch_idx] = leaf;
      to_hash = leaf;
    }
  }
  CHECK_PTR(to_hash);

  size_.fetch_add(1, std::memory_order_relaxed);
  // [generate_hashes_from()] unlocks the node locks we currently hold.
  generate_hashes_from(to_hash, /*locked_through=*/branch);
  return true;
}

// We are inserting [k] into the trie, and it conflicts with an extension node
// on the same path. This method resolves that conflict.
//
// Example:
// We are inserting [k = 0x1234] into the trie and it conflicts with an
// extension node with path [0x256]. In this case, we will replace the
// extension node with the following:
//
// First Extension Node: Path is [0x2] with child c. Child is the branch node
//                       below.
// Branch Node: Has a leaf for [k] as child [0x3]. Has the second extension
// node
//              below as as child [0x5].
// Second Extension Node: Path is [0x6]. The child is c.
Node *
MerkleTrie::insert_handle_extension(Node *extension, const Key &k, size_t idx,
                                    T &&obj)
{
  extension->rw_lock.lock_writer();

  CHECK_EQ(extension->type, Node::Type::kExtension);
  CHECK_EQ(extension->parent->type, Node::Type::kBranch);
  CHECK(!extension->path.empty());

  // [key_window] holds the portion of [k] that we are currently looking at.
  // In other words, we omit the prefix of [k] that corresponds to the nodes
  // in the trie we have already visited.
  //
  // We add 1 since one of the digits will be included in the
  // [extension->parent] node, which is always a branch node.
  const Key key_window = k.get_subarray(idx + 1, k.size());

  size_t common_range_length =
      get_longest_common_range_length(key_window, 0, extension->path, 0);
  size_t diverge_idx = common_range_length;

  // We are going to create a new branch node, with a leaf for [key_window] as
  // one child and the existing subtree as the other child.
  //
  // See the comments below. It is possible that we place [key_window]
  // directly into the branch. It is also possible that we omit part of the
  // existing subtree, too.
  Node *branch = nullptr;
  // We are going to reuse [extension] in the if/else bodies below, so we
  // cache some of the node contents since we need them after the node is
  // reused.
  //
  // [Node] has its copy constructor and copy assignment operator delete, so we
  // copy the fields individually.
  Node *const extension_cache_parent = extension->parent;
  const HexArray extension_cache_path = extension->path;
  Node *const extension_cache_child = extension->child[0];

  auto parent_branch_hex = std::to_integer<size_t>(k.get(idx));
  if (common_range_length) {
    // [key_window] and the extension node have a common range, so we are
    // going to break the extension node into two extension nodes:
    //
    // 1. The first extension node will have the path that is present in both
    // [key_window] and the existing extension node.
    //
    // 2. The second extension node will have the path that is not present in
    // [key_window]. This extension node will be a child of the new branch.
    //
    // Example:
    // If [key_window] has path [0x12ab] and [extension->path] is [0x1234],
    // then the following nodes will be created:
    //
    // First Extension Node: Path is [0x12].
    // Branch Node: A leaf for [key_window] is child [0xa] and the second
    //              extension node is child [0x3].
    // Second Extension Node: Path is [0x4].
    // Leaf Node: Path is [0xb].
    //
    // Note that the second extension node may have a path length of 1, in
    // which case we will omit the second extension node and add its child
    // directly as a child of the new branch. Example: If [key_window] has
    // path [0x1234] and [extension->path] is [0x1235], then the following
    // nodes will be created:
    //
    // First Extension Node: Path is [0x123].
    // Branch Node: A leaf for [key_window] is child [0x4]. The child of
    //              [extension] is child [0x5] in this branch.
    Node *first = Node::create_extension(/*parent=*/extension_cache_parent);
    CHECK(first->rw_lock.try_lock_writer());
    first->path = key_window.get_subarray(0, diverge_idx);

    // Place the first extension node as a child of the branch.
    extension_cache_parent->child[parent_branch_hex] = first;

    // Create the new branch node and add it as a child of the first extension
    // node.
    //
    // Note that we reuse [extension] below.
    branch = extension;
    branch->clear();
    branch->init_branch(/*parent=*/first);
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
    // In this case, we create a new branch and place that branch as child
    // [0x1] of the branch node mentioned above. Then we place the extension
    // node as one child of this new branch and we create a leaf node for
    // [key_window] and place the leaf as the other child of the new branch.
    //
    // Note that we reuse [extension] below.
    branch = extension;
    branch->clear();
    branch->init_branch(/*parent=*/extension_cache_parent);
    // We are reusing [extension] as [branch], so they are the same pointer.
    // Thus, we can technically omit the line below below, but we keep it for
    // clarity.
    extension_cache_parent->child[parent_branch_hex] = branch;
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
  CHECK_LT(diverge_idx, extension_cache_path.size());
  auto second_branch =
      std::to_integer<size_t>(extension_cache_path.get(diverge_idx));
  extension_cache_child->rw_lock.lock_writer();
  if (diverge_idx < extension_cache_path.size() - 1) {
    extension = Node::create_extension(/*parent=*/branch);
    // The second extension node has a path size larger than 1.
    branch->child[second_branch] = extension;

    // Add 1 to account for the branch node. We are placing the second extension
    // node as a child of the branch node, so the branch node accounts for 1 hex
    // digit in the path.
    extension->path =
        extension_cache_path.get_subarray(diverge_idx + 1,
                                          extension_cache_path.size());
    extension->child[0] = extension_cache_child;
    // [extension] was reused above, so [extension_cache_child->parent]
    // currently points to the reused node rather than the [extension] we just
    // allocated. Thus, we update the parent pointer to [extension].
    extension_cache_child->parent = extension;
    // Note that this second extension node is *not* on the path from the node
    // that will hold [key_window] to the root. Thus, when we rehash the nodes
    // on [key_window]'s path in the trie, we will not rehash this second
    // extension node. Thus, to ensure that the hashes are generated properly
    // in the trie, we need to hash this second extension node here.
    compute_node_hash(*extension);
  }
  else {
    // The second extension node would have a path size of 1, so we instead
    // place the second extension node's child directly as a child of the
    // branch.
    branch->child[second_branch] = extension_cache_child;
    extension_cache_child->parent = branch;
  }
  extension_cache_child->rw_lock.unlock();

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
  // Create new leaf node for [key_window] and add the leaf node as a child of
  // the new branch.
  Node *leaf = Node::create_leaf(/*parent=*/branch);
  CHECK(leaf->rw_lock.try_lock_writer());
  // Add 1 to account for the branch node. Since we are placing the leaf node
  // for [key_window] as a child of the branch, the branch accounts for 1 hex
  // digit in the path.
  leaf->path = key_window.get_subarray(diverge_idx + 1, key_window.size());
  leaf->value = std::move(obj);

  auto leaf_branch = std::to_integer<size_t>(key_window.get(diverge_idx));
  branch->child[leaf_branch] = leaf;
  return leaf;
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
MerkleTrie::insert_handle_leaf(Node *leaf, const Key &k, size_t idx, T &&obj)
{
  CHECK(leaf->rw_lock.try_lock_writer());

  CHECK_EQ(leaf->type, Node::Type::kLeaf);
  CHECK_EQ(leaf->parent->type, Node::Type::kBranch);

  // We add 1 since one of the digits will be included in the [leaf->parent]
  // node.
  auto parent_branch_digit = std::to_integer<size_t>(k.get(idx));
  Key key_window = k.get_subarray(idx + 1, k.size());

  // We create a branch that we will put both keys under.
  Node *branch = Node::create_branch(/*parent=*/leaf->parent);
  CHECK(branch->rw_lock.try_lock_writer());
  leaf->parent->child[parent_branch_digit] = branch;

  // Check to see if [key_window] and the path in [leaf] have any digits in
  // common. If they do, we create an extension node with the shared digits, and
  // then we place the new branch as a child of this new extension node.
  size_t common_range_length =
      get_longest_common_range_length(key_window, 0, leaf->path, 0);
  if (common_range_length) {
    size_t diverge_idx = common_range_length;
    Node *extension = Node::create_extension(/*parent=*/leaf->parent);
    CHECK(extension->rw_lock.try_lock_writer());
    extension->path = key_window.get_subarray(0, diverge_idx);

    leaf->parent->child[parent_branch_digit] = extension;
    extension->child[0] = branch;
    branch->parent = extension;

    // Now remove the extension path from these keys.
    key_window = key_window.get_subarray(diverge_idx, key_window.size());
    leaf->path = leaf->path.get_subarray(diverge_idx, leaf->path.size());
  }

  // Now we place both keys in the branch. Note that it is possible one of the
  // keys is shorter than the other. If so, it is possible the shorter key's
  // remaining path is empty, in which case it should be placed directly into
  // the branch rather than in a leaf node that is a child of the branch.
  const bool key_shorter = key_window.size() <= leaf->path.size();
  const Key &shorter_path = key_shorter ? key_window : leaf->path;
  const Key &longer_path = key_shorter ? leaf->path : key_window;
  T &&shorter_value =
      key_shorter
          ? obj
          : std::move(
                *leaf->value); // NOLINT(bugprone-unchecked-optional-access)
  T &&longer_value =
      key_shorter
          ? std::move(
                *leaf->value) // NOLINT(bugprone-unchecked-optional-access)
          : obj;

  if (shorter_path.empty()) {
    // The shorter key's remaining path is empty, so we should place the
    // shorter key directly into the branch.
    branch->value = std::move(shorter_value);
  }
  else {
    CHECK(!longer_path.empty());
    CHECK_NE(static_cast<uint8_t>(shorter_path.get(0)),
             static_cast<uint8_t>(longer_path.get(0)));
    // We place the two keys into their own leaf nodes and we make those leaf
    // nodes children of the branch.

    // Shorter key's leaf node. Note that we reuse [leaf] for the longer node
    // below, so we allocate a new node here.
    //
    // Note that I am deliberately allocating a new node first *before*
    // reusing [leaf]. The [shorter_*] and [longer_*] variables above are
    // references to elements in [leaf], so we cannot overwrite [leaf] until
    // we no longer need those references.
    Node *shorter_leaf = Node::create_leaf(/*parent=*/branch);
    // Unlike the other nodes, we do not need to call [shorter_path.get(0)]
    // before writing [shorter_leaf->path] because we are not reusing [leaf].
    // Thus, [shorter_path] will never be a reference to [shorter_leaf->path].
    // I still call it first though to match the style of the node creation
    // below.
    auto branch_hex = std::to_integer<size_t>(shorter_path.get(0));
    branch->child[branch_hex] = shorter_leaf;
    shorter_leaf->path = shorter_path.get_subarray(1, shorter_path.size());
    shorter_leaf->value = std::move(shorter_value);
    // We hash this leaf here, and then in the parent function, we hash all
    // nodes on the path from the leaf *created below* to the root. Note that
    // this leaf is not on that path, so we must hash it here.
    compute_node_hash(*shorter_leaf);
  }

  // Now we place the longer key into a leaf node that is a child of the
  // branch. Note that we reuse [leaf] rather than allocate a new node.
  //
  // We overwrite [leaf->path] below, so we need call [longer_path.get(0)]
  // first as [longer_path] may be a reference to [leaf->path].
  auto branch_hex = std::to_integer<size_t>(longer_path.get(0));
  branch->child[branch_hex] = leaf;
  leaf->path = longer_path.get_subarray(1, longer_path.size());
  leaf->value = std::move(longer_value);
  leaf->parent = branch;

  return leaf;
}

bool
MerkleTrie::remove(const Key &k)
{
  size_t idx;
  Node *existing_key;
  Node *branch;
  size_t num_children;
  Node *locked_through = nullptr;

  while (true) {
    idx = 0;
    existing_key = nullptr;

    branch = traverse_to_lowest_branch(/*path=*/k, &root_, idx, existing_key);
    if (!existing_key) {
      // [k] does not exist in the trie, so we cannot remove it.
      branch->rw_lock.unlock();
      return false;
    }
    CHECK_PTR(branch);
    CHECK_EQ(branch->type, Node::Type::kBranch);
    locked_through = branch;

    if (!branch->rw_lock.try_upgrade_to_writer_if_only_one()) {
      // We need to try again. We cannot block on acquiring the writer lock
      // because we could deadlock with another thread trying to do the same.
      //
      // We also cannot drop the lock and then acquire it as the writer because
      // it is possible that another thread is attempting to remove [k],
      // acquires the writer lock for [branch] before we do, and then removes
      // [k].
      existing_key->rw_lock.unlock();
      if (branch != existing_key) {
        branch->rw_lock.unlock();
      }
      continue;
    }

    num_children = get_num_children(*branch);
    if (branch != &root_ &&
        (num_children == 2 || (num_children == 1 && branch->value))) {
      CHECK_PTR(branch->parent);

      // We are going to remove this branch node below, so acquire the writer
      // lock for the nodes above us until we reach the next branch. The next
      // branch is either the parent, or if the parent is an extension, then the
      // grandparent.
      if (!branch->parent->rw_lock.try_lock_writer()) {
        // We do not block on acquiring the writer lock when moving up the trie
        // because we could deadlock with another thread that is acquiring
        // writer locks as it moves down the trie.
        existing_key->rw_lock.unlock();
        if (branch != existing_key) {
          branch->rw_lock.unlock();
        }
        continue;
      }
      locked_through = branch->parent;
      if (branch->parent->type != Node::Type::kBranch) {
        CHECK_PTR(branch->parent->parent);
        CHECK_EQ(branch->parent->type, Node::Type::kExtension);
        CHECK_EQ(branch->parent->parent->type, Node::Type::kBranch);

        if (!branch->parent->parent->rw_lock.try_lock_writer()) {
          // As explained above, we do not block on acquiring the writer lock
          // when we move up the trie.
          existing_key->rw_lock.unlock();
          if (branch != existing_key) {
            branch->rw_lock.unlock();
          }
          branch->parent->rw_lock.unlock();
          continue;
        }
        locked_through = branch->parent->parent;
      }
    }
    break;
  }

  if (existing_key == branch) {
    // [k] maps to a branch, so clear the branch's value.
    CHECK(branch->value.has_value());
    branch->value = std::nullopt;
  }
  else {
    // [existing_key != branch], so we know that the key is a leaf node which is
    // a child of a branch. Thus, we need to remove this leaf node.
    CHECK_EQ(existing_key->type, Node::Type::kLeaf);

    auto branch_hex = std::to_integer<size_t>(k.get(idx));
    CHECK_EQ(branch->child[branch_hex], existing_key);
    CHECK_GT(num_children, 0UL);

    // Remove [k]'s leaf as a child of the branch.
    branch->child[branch_hex] = nullptr;
    num_children--;
    delete existing_key;
  }

  Node *to_hash = nullptr;
  if (branch == &root_) { // NOLINT(bugprone-branch-clone)
    // If the branch is the root node, we need to keep it. Even if the branch
    // now has fewer than two KV pairs "under" it, there is no further
    // optimization possible.
    to_hash = branch;
  }
  else if (num_children > 1 || (num_children == 1 && branch->value)) {
    // The branch has multiple children, or there is exactly one child and
    // there is a value in the branch (so in other words, there are two KV
    // pairs "under" this branch). Thus, as the branch still has at least two
    // KV pairs "under" it, no further optimization is possible.
    to_hash = branch;
  }
  else {
    // The branch now has fewer than two KV pairs "under" it, so we can
    // optimize our trie by removing the branch.
    to_hash = remove_branch(k, branch, num_children);
  }

  size_.fetch_sub(1, std::memory_order_relaxed);
  CHECK_PTR(to_hash);
  // [generate_hashes_from()] unlocks the node locks we currently hold.
  generate_hashes_from(to_hash, locked_through);
  return true;
}

// Removes [branch] because it only has one KV pair "under" it, so it is no
// longer needed.
//
// static
Node *
MerkleTrie::remove_branch(const Key & /*k*/, Node *branch, size_t num_children)
{
  // We should only remove a branch if it has one KV pair "under" it.
  if (num_children) {
    CHECK_EQ(num_children, 1UL);
  }
  else {
    CHECK(branch->value.has_value());
  }
  // We should not remove the root node.
  CHECK_PTR(branch->parent);

  // It is possible that the branch's parent is an extension node. In this
  // case, we need to remove the extension node, too, and then we need to add
  // the extension node's path as a prefix to the child in the branch. We use
  // [cur_path] to hold the extension node's path (if needed).
  HexArray cur_path;

  // This is the hex digit in the parent branch that corresponds to the path
  // that [k] is on. We are removing [branch] from the trie, so we need to
  // know this hex digit so that we know which of the parent branch's child
  // pointers to update.
  size_t parent_branch_hex;

  // Check if the branch's parent is an extension node.
  if (branch->parent->type == Node::Type::kExtension) {
    Node *extension = branch->parent;
    // [extension] is the child of the parent branch, so we use it to set
    // [parent_branch_hex].
    CHECK(branch_get_child_index(*extension->parent, extension,
                                 parent_branch_hex));
    cur_path.append(branch->parent->path);

    // Remove the extension node as a child of the parent branch. Replace it
    // with [branch].
    extension->parent->child[parent_branch_hex] = branch;
    branch->parent = extension->parent;

    delete extension;
  }
  else {
    CHECK_EQ(branch->parent->type, Node::Type::kBranch);
    // The branch's parent is the parent branch itself. Thus, we use [branch]
    // to set [parent_branch_hex].
    CHECK(branch_get_child_index(*branch->parent, branch, parent_branch_hex));
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

    // The branch has a value, so place the value in a leaf with path size 0.
    //
    // We reuse the branch node below.
    std::optional<T> value = std::move(branch->value);
    Node *parent = branch->parent;
    new_child = branch;
    new_child->clear();
    new_child->init_leaf(parent);
    new_child->path = cur_path;
    new_child->value = std::move(value);
  }
  else {
    CHECK_PTR(child);
    CHECK_EQ(num_children, 1UL);
    CHECK(!branch->value.has_value());

    child->rw_lock.lock_writer();

    if (child->type == Node::Type::kBranch) {
      // The remaining child under [branch] is also a branch. Thus, we can
      // replace [branch] with an extension node. The path in the extension
      // node will be [cur_path] (to account for a possible extension node we
      // removed above) followed by the hex digit in [branch] that the
      // remaining child is under.
      cur_path.append(child_hex);
      CHECK_GE(cur_path.size(), 1UL);
      // We reuse the branch node below.
      new_child = branch;
      Node *parent = branch->parent;
      new_child->clear();
      new_child->init_extension(parent);
      new_child->path = cur_path;
      new_child->child[0] = child;
      child->parent = new_child;

      child->rw_lock.unlock();
    }
    else {
      // The remaining child under [branch] is either an extension node or a
      // leaf node. Either way, we will add a prefix to the path in the node.
      // The prefix will be [cur_path] (to account for a possible extension
      // node we removed above) followed by the hex digit in the branch that
      // the remaining child is under.
      cur_path.append(child_hex);

      // Add [child->path] to [cur_path] since we then overwrite [child->path]
      // entirely with [cur_path]. We want to make sure the existing path in
      // [child] remains.
      cur_path.append(child->path);
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

// static
template<typename NodeType>
void
MerkleTrie::walk(
    NodeType *root, const HexArray &root_prefix,
    const std::function<void(const HexArray &, NodeType &, NodeType *)> &f)
{
  static_assert(std::is_same_v<NodeType, Node> ||
                    std::is_same_v<NodeType, const Node>,
                "[NodeType] must be [Node] or [const Node].");

  // We perform a post-order traversal iteratively. We use iteration rather than
  // recursion to avoid exceeding the stack size when traversing deep trees.

  // We use this type along with the data structures below to track when we are
  // in the tree.
  struct CheckInfo {
    // The node we are currently looking at.
    NodeType *node;
    // The parent of this node.
    NodeType *parent;
    // The path that maps to this node.
    HexArray path;
    // This is only relevant for branch and extension nodes. When we first
    // encounter these nodes in an iterative loop, we cannot *immediately*
    // "visit" them (i.e., execute [f] on them)--we need to visit all of their
    // children first.
    //
    // A branch or extension node will be left in the stack until all of its
    // children have been visited. When this node pushes one of its children
    // onto the stack, the node waits until the child is popped off the stack
    // before pushing the next child. Thus, the node will not enqueue the next
    // child until a subsequent iteration, and thus we need to track this child
    // so that the node knows which child to push at that point.
    //
    // We use [next_child_to_visit] to track which child should be pushed next
    // for [node].
    size_t next_child_to_visit;
  };

  std::stack<CheckInfo> path;
  // Push the root node onto the stack.
  path.push(CheckInfo{.node = root,
                      .parent = nullptr,
                      .path = root_prefix,
                      .next_child_to_visit = 0});

  while (!path.empty()) {
    CheckInfo &info = path.top();

    if (!info.node) {
      path.pop();
      continue;
    }

    switch (info.node->type) {
    case Node::Type::kBranch:
      CHECK_LE(info.next_child_to_visit, info.node->child.size());
      if (info.next_child_to_visit == info.node->child.size()) {
        // We have visited all of the branch's children, so now we can "visit"
        // (i.e., call [f] on) the branch.
        f(info.path, *info.node, info.parent);
      }
      else {
        // Visit the next child.

        // Append the branch path we are taking to [info.path] so that it holds
        // the path to the child. For example, if [info.next_child_to_visit] is
        // 0x3, then we know we will next visit the child at index 0x3, so we
        // append 0x3 to [info.path].
        info.path.append(std::byte(info.next_child_to_visit));
        path.push({.node = info.node->child[info.next_child_to_visit],
                   .parent = info.node,
                   .path = info.path,
                   .next_child_to_visit = 0});
        // Now change [info.path] back to holding the path to the branch node.
        info.path.remove_from_back(1);
        info.next_child_to_visit++;
        continue;
      }
      break;
    case Node::Type::kExtension:
      // An extension node always has exactly one child. It is not possible for
      // an extension node to have no children.
      CHECK_LE(info.next_child_to_visit, 1UL);
      if (info.next_child_to_visit == 1) {
        // We have visited the extension node's one child, so now we can visit
        // the extension node.
        f(info.path, *info.node, info.parent);
      }
      else {
        // Append the extension node's path to [info.path] so that [info.path]
        // holds the path to the child when we push the child.
        info.path.append(info.node->path);
        path.push({.node = info.node->child[0],
                   .parent = info.node,
                   .path = info.path,
                   .next_child_to_visit = 0});
        // Now change [info.path] back to holding the path to the extenion node.
        info.path.remove_from_back(info.node->path.size());
        info.next_child_to_visit++;
        continue;
      }
      break;
    case Node::Type::kLeaf:
      // A leaf node has no children, so we can visit a leaf immediately upon
      // encountering it.
      CHECK_EQ(info.next_child_to_visit, 0UL);

      // Append the leaf's path to [info.path] so that the full path is passed
      // to [f].
      info.path.append(info.node->path);
      f(info.path, *info.node, info.parent);
      // No need to remove the portion of the path we just appended above
      // because we pop the leaf anyway below, so we do not access the leaf
      // after this point.
      break;
    }

    path.pop();
  }
}

// static
Node *
MerkleTrie::traverse_to_lowest_branch(const HexArray &path, Node *root,
                                      size_t &idx, Node *&existing_key)
{
  CHECK_PTR(root);
  CHECK_LE(idx, path.size());

  Node *const original_root = root;
  const size_t original_idx = idx;

  Node *prev_root = nullptr;
  while (true) {
    if (!root->rw_lock.try_lock_reader()) {
      // We are conflicting with a writer that is moving up the trie. We defer
      // to the writer, so unlock [prev_root] and try again.
      if (prev_root) {
        prev_root->rw_lock.unlock();
      }

      root = original_root;
      prev_root = nullptr;
      idx = original_idx;
      continue;
    }

    switch (root->type) {
    case Node::Type::kBranch:
    {
      if (idx == path.size()) {
        if (root->value) {
          // [path] maps to this branch node.
          existing_key = root;
        }
        if (prev_root) {
          prev_root->rw_lock.unlock();
        }
        return root;
      }

      auto branch_hex = std::to_integer<size_t>(path.get(idx));
      Node *next = root->child[branch_hex];
      if (next) {
        // There is a child that corresponds to the next hex digit in [path],
        // so continue searching. We add 1 to [idx] since the branch
        // corresponds to one hex digit in the path.
        idx++;
        if (prev_root) {
          prev_root->rw_lock.unlock();
        }
        prev_root = root;
        root = next;
        continue;
      }
      // There is no child, so we have reached the bottom of the trie.
      if (prev_root) {
        prev_root->rw_lock.unlock();
      }
      return root;
      break;
    }
    case Node::Type::kExtension:
    {
      // We check [idx + root->path.size() <= path.size()]. Note that when
      // these two values are equal, the path maps to the child node, which is
      // always a branch.
      if (idx + root->path.size() <= path.size() &&
          path.get_subarray(idx, idx + root->path.size()) == root->path) {
        idx += root->path.size();
        CHECK_PTR(root->child[0]);
        if (prev_root) {
          prev_root->rw_lock.unlock();
        }
        prev_root = root;
        root = root->child[0];
        continue;
      }
      // The extension node's path does not match [key], so we can stop moving
      // down the trie.
      //
      // We are going to return the parent node, which is always a branch, so
      // subtract 1 from [idx] since the branch corresponds to one hex digit
      // in the path.
      idx--;
      root->rw_lock.unlock();
      return prev_root;
      break;
    }
    case Node::Type::kLeaf:
      if (path.get_subarray(idx, path.size()) == root->path) {
        // [path] maps to this leaf node.
        existing_key = root;
      }
      else {
        root->rw_lock.unlock();
      }
      // We are going to return the parent node, which is always a branch, so
      // subtract 1 from [idx] since the branch corresponds to one hex digit
      // in the path.
      idx--;
      return prev_root;
      break;
    default:
      CHECK(false);
      return nullptr;
      break;
    }
  }

  CHECK(false);
  return nullptr;
}

void
MerkleTrie::generate_hashes_from(Node *n, Node *locked_through) const
{
  if (!should_hash_nodes_on_each_op_) {
    return;
  }

  CHECK_PTR(locked_through);
  bool locked = true;
  while (true) {
    CHECK_PTR(n);
    CHECK(n->rw_lock.is_writer_locked_relaxed());
    if (n == locked_through) {
      locked = false;
    }

    // TODO(jhumphri): We need to lock all of the children before calling this.
    n->hash = n->compute_node_hash();

    // TODO(jhumphri): I think the current writer could produce a stale hash if
    // we return in the if statement below.
    //
    // I think that a writer traveling down the tree needs to defer to a writer
    // traveling up the tree. The former can restart its work whereas the latter
    // cannot. If the latter tries to restart its work, it will start at [n]
    // which may no longer exist if the KV pair in [n] was removed after we
    // released the lock for [n].
    if (!n->parent || (!locked && !n->parent->rw_lock.try_lock_writer())) {
      // We are conflicting with a writer for [n]. We can stop generating hashes
      // because we know the other thread will generate a hashes for all nodes
      // from [n] to the root (inclusive).
      n->rw_lock.unlock();
      return;
    }
    Node *parent = n->parent;
    n->rw_lock.unlock();
    // It is technically safe to do [n = n->parent] below because a thread needs
    // to hold both the writer lock for [n] and the writer lock for [n->parent]
    // in order to overwrite [n->parent], but we still cache [n->parent] in
    // [parent] to avoid future issues.
    n = parent;
  }
}

void
MerkleTrie::compute_node_hash(Node &node) const
{
  if (should_hash_nodes_on_each_op_) {
    node.hash = node.compute_node_hash();
  }
}

void
MerkleTrie::generate_all_hashes()
{
  HexArray prefix;
  auto f = [&](const HexArray & /*path*/, Node &node, Node * /*parent*/) {
    node.hash = node.compute_node_hash();
  };
  walk<Node>(&root_, prefix, f);
}

void
MerkleTrie::free_all_nodes(Node *root) const
{
  HexArray prefix;
  auto f = [&](const HexArray & /*path*/, Node &node, Node * /*parent*/) {
    // [root_] is a [MerkleTrie] class member of type [Node]. It is not a
    // pointer to a [Node] value on the heap, so we cannot free the root node.
    if (&node != &root_) {
      delete &node;
    }
  };
  walk<Node>(root, prefix, f);
}

} // namespace hogd
