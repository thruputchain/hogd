// merkle.h - provides a merkle patricia trie

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
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

  MerkleTrie() {}
  ~MerkleTrie() = default;

  // disable copy and support move.
  MerkleTrie(const MerkleTrie &) = delete;
  MerkleTrie &operator=(const MerkleTrie &) = delete;
  MerkleTrie(MerkleTrie &&) noexcept = default;
  MerkleTrie &operator=(MerkleTrie &&) noexcept = default;

  // TODO: For all of the methods below, should they include a mutex as well (in
  // a pair)?

  std::string &operator[](const Key &key)
  {
    static std::string kEmpty = "";

    assert(false);
    return kEmpty;
  }

  std::string *find(const Key &key)
  {
    static std::string kEmpty = "";

    assert(false);
    return &kEmpty;
  }

  void insert(const Key &k, T &&obj)
  {
    if (!root_) {
      // No root node exists, so create it.
      root_ = new Node;
      root_->type = NodeType::kLeaf;
      root_->leaf_path = k;
      root_->leaf_value = std::move(obj);

      root_->hash = GetNodeHash(*root_);
      return;
    }

    // A root node already exists. If the root is a leaf node, then we need to
    // convert it to a branch node and build the trie.
    //
    // TODO: Optimize with extension nodes.
  }

  void insert(const Key &k, const T &obj)
  {
    assert(false);
  }

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

  size_t size()
  {
    assert(false);
    return 0;
  }

  void clear() { assert(false); }

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
  enum class NodeType {
    kBranch,
    kExtension,
    kLeaf
  };

  struct Node {
    NodeType type;
    SHA256Hash hash;

    // TODO: Optimize the variables below with a union or different node types.
    // TODO: Optimize the vectors below--use something other than a vector.

    // Branches.
    std::vector<Node*> branch_next;
    std::string branch_value;

    // Extensions.
    std::vector<std::byte> extension_path;
    Node* extension_key;

    // Leaves.
    std::vector<std::byte> leaf_path;
    std::string leaf_value;
  };

  SHA256Hash GetNodeHash(const Node& node) {
    // TODO: Implement.
    static std::array<unsigned char, 32> tmp = {0};
    return tmp;
  }

  Node *root_ = nullptr;
};

} // namespace hogd
