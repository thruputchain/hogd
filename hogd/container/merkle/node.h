// node.h - the node in a merkle patricia trie

#pragma once

#include <gtest/gtest.h>

#include <vector>

#include "hogd/base/types.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/crypto/crypto.h"

namespace hogd {

class Node {
public:
  // The maximum number of children of a branch node. The trie branches on a hex
  // digit. There are 16 different hex digits, so we need 16 different child
  // slots.
  static constexpr size_t kMaxNumBranches = 16;
  static_assert(0xF + 1 == kMaxNumBranches);
  // We also use the [child] array to hold the child of an extension node, so
  // ensure [kMaxNumBranches] is at least 1.
  static_assert(kMaxNumBranches);

  enum class Type { kBranch, kExtension, kLeaf };

  // This is a raw representation of the node, used to hold the node's contents
  // after it has been read from an encoding.
  //
  // This [Raw] type is identical to [Node] apart from the children, which are
  // hashes in the [Raw] type and pointers in the [Node] type. We typically only
  // read node encodings when verifying a Merkle Trie Proof, and for branches in
  // and extensions in particular, their corresponding node encodings in the
  // proof directly include the hashes of their children. We write these hashes
  // to [child_hashes] in the [Raw] type.
  //
  // Alternatively, we could just regenerate [Node] instances when verifying a
  // proof rather than use this [Raw] type, but for branches and extensions, we
  // would need to allocate children nodes and write pointers to them in the
  // [child] array in the [Node] type. Handling this allocation seems overly
  // complicated, so we use this [Raw] type instead.
  struct Raw {
    Type type;
    SHA256Hash hash;
    HexArray path;
    std::optional<std::string> value;
    std::array<SHA256Hash, kMaxNumBranches> child_hashes;
  };

  Type type;
  SHA256Hash hash;
  Node *parent;

  // TODO: Optimize the variables below with a union or different node types.

  // Used by extensions and leaves.
  HexArray path;

  // Used by branches and leaves.
  // TODO: Should we be able to place values in extension nodes, too? For
  // example, the [TwoLeavesAndOneBranch] test would produce a more efficient
  // trie if we could do this.
  std::optional<std::string> value;

  // Used by branches and extensions.
  //
  // For branches, we branch on a hex digit and up to all [kMaxNumBranches]
  // slots may be filled.
  //
  // For extensions, only the first slot is filled. The other
  // [kMaxNumBranches - 1] slots are unused.
  std::array<Node *, kMaxNumBranches> child;

  void InitBranch(Node *parent)
  {
    type = Type::kBranch;
    this->parent = parent;
  }

  void InitExtension(Node *parent)
  {
    type = Type::kExtension;
    this->parent = parent;
  }

  void InitLeaf(Node *parent)
  {
    type = Type::kLeaf;
    this->parent = parent;
  }

  static Node *CreateBranch(Node *parent)
  {
    Node *node = new Node;
    node->Clear();
    node->InitBranch(parent);
    return node;
  }

  static Node *CreateExtension(Node *parent)
  {
    Node *node = new Node;
    node->Clear();
    node->InitExtension(parent);
    return node;
  }

  static Node *CreateLeaf(Node *parent)
  {
    Node *node = new Node;
    node->Clear();
    node->InitLeaf(parent);
    return node;
  }

  // Returns an encoding of the node as a byte vector.
  std::vector<std::byte> GetNodeEncoding();

  // This reads a raw node representation from [encodings] starting at index
  // [idx].
  static bool GetNodeFromEncoding(const std::vector<std::byte> &encodings,
                                  size_t &idx, Node::Raw &node);

  SHA256Hash ComputeNodeHash() { return sha256(GetNodeEncoding()); }

  void Clear()
  {
    // [Node] is not a trivally copyable type (due to the [std::string] in the
    // [value] field), so we cannot just memset() it to all zeroes. Instead, we
    // assign each field individually.
    memset(&hash, 0, sizeof(hash));
    parent = nullptr;
    path.Clear();
    value = std::nullopt;
    memset(child.data(), 0, child.size() * sizeof(child[0]));
  }

  std::string str() const;

  friend std::ostream &operator<<(std::ostream &os, const Node &node)
  {
    return os << node.str();
  }

  static std::string GetTypeString(Type type)
  {
    switch (type) {
    case Type::kBranch:
      return "Branch";
    case Type::kExtension:
      return "Extension";
    case Type::kLeaf:
      return "Leaf";
    default:
      return "Unknown-Node-Type";
    }
  }
  friend std::ostream &operator<<(std::ostream &os, const Type type)
  {
    return os << GetTypeString(type);
  }

private:
  // These encode the node into a byte vector.
  void GetBranchEncoding(std::vector<std::byte> &encoding) const;
  void GetExtensionEncoding(std::vector<std::byte> &encoding) const;
  void GetLeafEncoding(std::vector<std::byte> &encoding) const;

  // These write a string representation of the node to [oss].
  void GetBranchString(std::ostringstream &oss) const;
  void GetExtensionString(std::ostringstream &oss) const;
  void GetLeafString(std::ostringstream &oss) const;
};
} // namespace hogd
