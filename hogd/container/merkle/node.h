// node.h - the node in a merkle patricia trie

#pragma once

#include <array>
#include <cstddef>
#include <cstring>

#include <format>
#include <optional>
#include <ostream>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/rw_lock.h"
#include "hogd/crypto/crypto.h"
#include "types.hh"

namespace hogd {

// This is a node in a Merkle Patricia trie.
class Node {
public:
  // The maximum number of children of a branch node. The trie branches on a hex
  // digit. There are 16 different hex digits, so we need 16 different child
  // slots.
  static constexpr size_t kMaxNumBranches = 16;
  static_assert(0xF + 1 == kMaxNumBranches);
  // We also use the [child] array to hold the child of an extension node, so
  // ensure [kMaxNumBranches] is at least 1.
  static_assert(kMaxNumBranches != 0U);

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

  Node() = default;
  // Disable copy and move.
  Node(const Node &) = delete;
  Node &operator=(const Node &) = delete;
  Node(Node &&) noexcept = delete;
  Node &operator=(Node &&) noexcept = delete;

  RwLock rw_lock;
  Type type;
  SHA256Hash hash;
  Node *parent;

  // TODO(jhumphri): Optimize the variables below with a union or different node
  // types.

  // Used by extensions and leaves.
  HexArray path;

  // Used by branches and leaves.
  // TODO(jhumphri): Should we be able to place values in extension nodes, too?
  // For example, the [TwoLeavesAndOneBranch] test would produce a more
  // efficient trie if we could do this.
  std::optional<std::string> value;

  // Used by branches and extensions.
  //
  // For branches, we branch on a hex digit and up to all [kMaxNumBranches]
  // slots may be filled.
  //
  // For extensions, only the first slot is filled. The other
  // [kMaxNumBranches - 1] slots are unused.
  std::array<Node *, kMaxNumBranches> child;

  void init_branch(Node *parent)
  {
    type = Type::kBranch;
    this->parent = parent;
  }

  void init_extension(Node *parent)
  {
    type = Type::kExtension;
    this->parent = parent;
  }

  void init_leaf(Node *parent)
  {
    type = Type::kLeaf;
    this->parent = parent;
  }

  static Node *create_branch(Node *parent)
  {
    // TODO(jhumphri): Use unique_ptr<> rather than [new] (here and elsewhere).
    Node *node = new Node;
    node->clear();
    node->init_branch(parent);
    return node;
  }

  static Node *create_extension(Node *parent)
  {
    Node *node = new Node;
    node->clear();
    node->init_extension(parent);
    return node;
  }

  static Node *create_leaf(Node *parent)
  {
    Node *node = new Node;
    node->clear();
    node->init_leaf(parent);
    return node;
  }

  // Returns an encoding of the node as a byte vector.
  std::vector<std::byte> get_node_encoding() const;

  // This reads a raw node representation from [encodings] starting at index
  // [idx].
  static bool get_node_from_encoding(const std::vector<std::byte> &encodings,
                                     size_t &idx, Node::Raw &node);

  SHA256Hash compute_node_hash() const { return sha256(get_node_encoding()); }

  void clear()
  {
    // [Node] is not a trivally copyable type (due to the [std::string] in the
    // [value] field), so we cannot just memset() it to all zeroes. Instead, we
    // assign each field individually.
    memset(&hash, 0, sizeof(hash));
    parent = nullptr;
    path.clear();
    value = std::nullopt;
    memset(static_cast<void *>(child.data()), 0,
           child.size() *
               sizeof(child[0])); // NOLINT(bugprone-sizeof-expression)
  }

  std::string str() const;

  friend std::ostream &operator<<(std::ostream &os, const Node &node)
  {
    return os << node.str();
  }

  static std::string get_type_string(Type type)
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
    return os << get_type_string(type);
  }

private:
  // These encode the node into a byte vector.
  void get_branch_encoding(std::vector<std::byte> &encoding) const;
  void get_extension_encoding(std::vector<std::byte> &encoding) const;
  void get_leaf_encoding(std::vector<std::byte> &encoding) const;

  // These write a string representation of the node to [oss].
  void get_branch_string(std::ostringstream &oss) const;
  void get_extension_string(std::ostringstream &oss) const;
  void get_leaf_string(std::ostringstream &oss) const;
};
} // namespace hogd

// We need to define the formatters in the [std] namespace.
template<> struct std::formatter<hogd::Node> : std::formatter<std::string> {
  auto format(const hogd::Node &node, std::format_context &ctx) const
  {
    std::ostringstream oss;
    oss << node;
    return std::formatter<std::string>::format(oss.str(), ctx);
  }
};

// Also need to define a formatter for [Node *], not just [Node].
template<> struct std::formatter<hogd::Node *> : std::formatter<std::string> {
  auto format(const hogd::Node *node, std::format_context &ctx) const
  {
    std::ostringstream oss;
    oss << node;
    return std::formatter<std::string>::format(oss.str(), ctx);
  }
};

template<>
struct std::formatter<hogd::Node::Type> : std::formatter<std::string> {
  auto format(const hogd::Node::Type &type, std::format_context &ctx) const
  {
    std::ostringstream oss;
    oss << type;
    return std::formatter<std::string>::format(oss.str(), ctx);
  }
};
