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

  MerkleTrie() {
    memset(&root_hash_, 0, sizeof(root_hash_));
    printf("sizeof(root_hash_) = %zu\n", sizeof(root_hash_));
    for (size_t i = 0; i < sizeof(root_hash_); i++) {
      printf("root_hash_[%zu] = %x\n", i, root_hash_[i]);
    }
  }
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

  std::string *insert(const Key &k, T &&obj)
  {
    static std::string kEmpty = "";

    assert(false);
    return &kEmpty;
  }

  std::string *insert(const Key &k, const T &obj)
  {
    static std::string kEmpty = "";

    assert(false);
    return &kEmpty;
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

  SHA256Hash root_hash_;
  Node *root_ = nullptr;
};

} // namespace hogd
