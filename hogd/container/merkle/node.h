// node.h - the node in a merkle patricia trie

#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>

#include <format>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/allocator.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/rw_lock.h"
#include "hogd/crypto/crypto.h"
#include "types.hh"

namespace hogd {

// This is a node in a Merkle Patricia trie.
struct Node {
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
  // read node encodings when verifying a Merkle Trie Proof, and for branches
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
    std::optional<SHA256Hash> value_hash;
    std::array<SHA256Hash, kMaxNumBranches> child_hashes;
  };

  struct BranchPayload {
    std::optional<SHA256Hash> value_hash;
    std::array<Node *, kMaxNumBranches> child{};
  };

  struct ExtensionPayload {
    HexArray path;
    Node *child = nullptr;
  };

  struct LeafPayload {
    HexArray path;
    std::optional<SHA256Hash> value_hash;
  };

  union Payload {
    BranchPayload branch;
    ExtensionPayload extension;
    LeafPayload leaf;

    Payload() {}
    ~Payload() {}
  };

  // A node is default constructed as a branch (see [Type type{Type::kBranch}]
  // below).
  Node()
  {
    std::construct_at(&payload_.branch);
    rw_lock.reset();
    hash_lock.reset();
    clear_common();
  }

  ~Node() { destroy_payload(); }
  // Disable copy and move.
  Node(const Node &) = delete;
  Node &operator=(const Node &) = delete;
  Node(Node &&) noexcept = delete;
  Node &operator=(Node &&) noexcept = delete;

  RwLock rw_lock;
  Type type{Type::kBranch};
  RwLock hash_lock;
  SHA256Hash hash;
  Node *parent;
  Payload payload_;

  void init_branch(Node *parent)
  {
    if (type != Type::kBranch) {
      destroy_payload();
      std::construct_at(&payload_.branch);
    }
    clear_common();
    payload_.branch.value_hash = std::nullopt;
    payload_.branch.child.fill(nullptr);
    type = Type::kBranch;
    this->parent = parent;
  }

  void init_extension(Node *parent)
  {
    if (type != Type::kExtension) {
      destroy_payload();
      std::construct_at(&payload_.extension);
    }
    clear_common();
    payload_.extension.path.clear();
    payload_.extension.child = nullptr;
    type = Type::kExtension;
    this->parent = parent;
  }

  void init_leaf(Node *parent)
  {
    if (type != Type::kLeaf) {
      destroy_payload();
      std::construct_at(&payload_.leaf);
    }
    clear_common();
    payload_.leaf.path.clear();
    payload_.leaf.value_hash = std::nullopt;
    type = Type::kLeaf;
    this->parent = parent;
  }

  static Node *create_branch(Node *parent, Allocator<Node> &allocator)
  {
    // TODO(jhumphri): Use unique_ptr<> rather than [new] (here and elsewhere).
    Node *node = allocator.alloc();
    CHECK_PTR(node);

    if (allocator.type() == Allocator<Node>::Type::kBump) {
      // The bump allocator does not construct the node, whereas [new Node]
      // does. Thus, we need to manually construct the node.
      std::construct_at(node);
    }
    node->clear();
    node->rw_lock.reset();
    node->hash_lock.reset();
    node->init_branch(parent);
    return node;
  }

  static Node *create_extension(Node *parent, Allocator<Node> &allocator)
  {
    Node *node = allocator.alloc();
    CHECK_PTR(node);

    if (allocator.type() == Allocator<Node>::Type::kBump) {
      // The bump allocator does not construct the node, whereas [new Node]
      // does. Thus, we need to manually construct the node.
      std::construct_at(node);
    }
    node->clear();
    node->rw_lock.reset();
    node->hash_lock.reset();
    node->init_extension(parent);
    return node;
  }

  static Node *create_leaf(Node *parent, Allocator<Node> &allocator)
  {
    Node *node = allocator.alloc();
    CHECK_PTR(node);

    if (allocator.type() == Allocator<Node>::Type::kBump) {
      // The bump allocator does not construct the node, whereas [new Node]
      // does. Thus, we need to manually construct the node.
      std::construct_at(node);
    }
    node->clear();
    node->rw_lock.reset();
    node->hash_lock.reset();
    node->init_leaf(parent);
    return node;
  }

  // Returns an encoding of the node as a byte vector.
  std::vector<std::byte> &get_node_encoding() const;

  // This reads a raw node representation from [encodings] starting at index
  // [idx].
  static bool get_node_from_encoding(const std::vector<std::byte> &encodings,
                                     size_t &idx, Node::Raw &node);

  SHA256Hash compute_node_hash() const;

  void clear()
  {
    destroy_payload();
    std::construct_at(&payload_.branch);
    type = Type::kBranch;
    clear_common();
    payload_.branch.value_hash = std::nullopt;
    payload_.branch.child.fill(nullptr);
  }

  std::optional<SHA256Hash> &value_hash()
  {
    switch (type) {
    case Type::kBranch:
      return payload_.branch.value_hash;
    case Type::kLeaf:
      return payload_.leaf.value_hash;
    default:
      CHECK(false);
      return payload_.branch.value_hash;
    }
  }

  const std::optional<SHA256Hash> &value_hash() const
  {
    switch (type) {
    case Type::kBranch:
      return payload_.branch.value_hash;
    case Type::kLeaf:
      return payload_.leaf.value_hash;
    default:
      CHECK(false);
      return payload_.branch.value_hash;
    }
  }

  HexArray &path()
  {
    switch (type) {
    case Type::kExtension:
      return payload_.extension.path;
    case Type::kLeaf:
      return payload_.leaf.path;
    default:
      CHECK(false);
      return payload_.extension.path;
    }
  }

  const HexArray &path() const
  {
    switch (type) {
    case Type::kExtension:
      return payload_.extension.path;
    case Type::kLeaf:
      return payload_.leaf.path;
    default:
      CHECK(false);
      return payload_.extension.path;
    }
  }

  std::array<Node *, kMaxNumBranches> &branch_children()
  {
    CHECK(type == Type::kBranch);
    return payload_.branch.child;
  }

  const std::array<Node *, kMaxNumBranches> &branch_children() const
  {
    CHECK(type == Type::kBranch);
    return payload_.branch.child;
  }

  Node *&extension_child()
  {
    CHECK(type == Type::kExtension);
    return payload_.extension.child;
  }

  Node *extension_child() const
  {
    CHECK(type == Type::kExtension);
    return payload_.extension.child;
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
  void clear_common()
  {
    memset(&hash, 0, sizeof(hash));
    parent = nullptr;
  }

  void destroy_payload()
  {
    switch (type) {
    case Type::kBranch:
      std::destroy_at(&payload_.branch);
      break;
    case Type::kExtension:
      std::destroy_at(&payload_.extension);
      break;
    case Type::kLeaf:
      std::destroy_at(&payload_.leaf);
      break;
    }
  }

  // These encode the node into a byte vector.
  void get_branch_encoding(std::vector<std::byte> &encoding) const;
  void get_extension_encoding(std::vector<std::byte> &encoding) const;
  void get_leaf_encoding(std::vector<std::byte> &encoding) const;

  // These write a string representation of the node to [oss].
  void get_branch_string(std::ostringstream &oss) const;
  void get_extension_string(std::ostringstream &oss) const;
  void get_leaf_string(std::ostringstream &oss) const;
} __attribute__((aligned(64)));

static_assert(sizeof(Node) < 384);

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

// Also need to define a formatter for [const Node *].
template<>
struct std::formatter<const hogd::Node *> : std::formatter<std::string> {
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
