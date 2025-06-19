// node.h - the node in a merkle patricia trie

#pragma once

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
#include "hogd/base/compiler.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/crypto/crypto.h"
#include "types.hh"

namespace hogd {

class Node {
public:
  enum class Type { kBranch, kExtension, kLeaf };
  struct Raw {
    Type type;
    SHA256Hash hash;
    HexArray path;
    std::optional<std::string> value;
    std::array<SHA256Hash, 16> child_hashes;
  };

  Type type;
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
  // trie if we could do this.
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

  std::vector<std::byte> GetNodeEncoding();
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
  void GetBranchString(std::ostringstream &oss) const;
  void GetExtensionString(std::ostringstream &oss) const;
  void GetLeafString(std::ostringstream &oss) const;

  void GetBranchEncoding(std::vector<std::byte> &encoding) const;
  void GetExtensionEncoding(std::vector<std::byte> &encoding) const;
  void GetLeafEncoding(std::vector<std::byte> &encoding) const;
};
} // namespace hogd
