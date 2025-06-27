// merkle.h - provides a merkle patricia trie

#pragma once

#include <cstddef>
#include <functional>

#include <format>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/merkle/arena.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/node.h"
#include "types.hh"

// TODO(jhumphri): Support template types. For now, the keys are [HexArray]'s
// and the values are strings.
namespace hogd {

// This is a Merkle Patricia Trie. It holds KV pairs and can efficiently
// prove/verify whether a KV pair is in the trie.
//
// Example:
// MerkleTrie trie;
//
// HexArray key("1234");
// std::string value = "hello";
// trie.insert(key, value);
//
// std::pair<SHA256Hash, std::vector<std::byte>> proof = trie.get_proof();
//
// This returns true:
// trie.prove(proof.first, proof.second, key, value);
//
// This returns false:
// HexArray different_key("12");
// trie.prove(proof.first, proof.second, different_key, value);
//
// trie.remove(key);
class MerkleTrie {
public:
  // Type definitions.
  using Key = HexArray;
  using T = std::string;

  MerkleTrie() : arena_(/*alloc_size=*/sizeof(Node)) { init_root_node(); }

  ~MerkleTrie() // NOLINT(bugprone-exception-escape)
  {
    free_all_nodes(&root_);
  }

  // Disable copy and move.
  MerkleTrie(const MerkleTrie &) = delete;
  MerkleTrie &operator=(const MerkleTrie &) = delete;
  MerkleTrie(MerkleTrie &&) noexcept = delete;
  MerkleTrie &operator=(MerkleTrie &&) noexcept = delete;

  std::string str() const;
  friend std::ostream &operator<<(std::ostream &os, const MerkleTrie &trie)
  {
    return os << trie.str();
  }

  void PrintStats()
  {
    printf("Root:\n");
    root_.rw_lock.PrintStats();

    printf("\n\nLayer 1:\n");
    std::vector<Node *> layer_2;
    for (Node *child : root_.child) {
      if (!child) {
        continue;
      }

      child->rw_lock.PrintStats();
      if (child->type == Node::Type::kBranch) {
        for (Node *l2_child : child->child) {
          layer_2.push_back(l2_child);
        }
      }
      else if (child->type == Node::Type::kExtension) {
        layer_2.push_back(child->child[0]);
      }
    }

    printf("\n\nLayer 2:\n");
    for (Node *child : layer_2) {
      if (!child) {
        continue;
      }

      child->rw_lock.PrintStats();
    }
  }

  // Basic sanity checks on the trie.
  bool check() { return check_helper(&root_); }

  // Returns the hash of the root node in the trie.
  SHA256Hash get_root_hash()
  {
    // It is fine to spin on [try_lock_reader()] because we know the root node
    // is never deleted by a writer.
    while (!root_.rw_lock.try_lock_reader()) {
      // TODO(jhumphri): Do not use x86 instructions.
      asm volatile("pause");
    }

    SHA256Hash ret = root_.hash;
    root_.rw_lock.unlock();
    return ret;
  }

  // Returns a proof that the KV pair with key [k] is in the trie.
  std::pair<SHA256Hash, std::vector<std::byte>> get_proof(const Key &k);

  // Returns a proof that the KV pair with key [k] is in the trie. The second
  // element in the pair is a vector of encodings for each node. This function
  // separates each node encoding into its own index, whereas the [get_proof()]
  // function above returns the encodings concatted together.
  std::pair<SHA256Hash, std::vector<std::vector<std::byte>>>
  get_proof_by_node(const Key &k);

  // Returns true if the KV pair with key [k] and value [value] is in the trie.
  // [root_hash] is the hash of the root node and [encodings] is a vector of the
  // node encodings for all nodes on the path of this KV pair in the trie. Note
  // that if you call [get_proof()] above, [root_hash] is the first element in
  // the returned pair and [encodings] is the second element. If you call
  // [get_proof_by_node()], [root_hash] is the first element in the returned
  // pair and [encodings] is the flattened vector formed from the second
  // element.
  static bool prove(const SHA256Hash &root_hash,
                    const std::vector<std::byte> &encodings, const Key &k,
                    const T &value);

  // Finds the value for key [k]. If key [k] does not exist in the trie, returns
  // a nullptr.
  //
  // We do not guarantee pointer stability for the value. If the trie is mutated
  // while the caller holds the returned pointer, the pointer may no longer be
  // valid.
  T *find(const Key &k);

  // Inserts a KV pair with key [k] and value [obj] into the trie. You must pass
  // [obj] to this method with [std::move()].
  bool insert(const Key &k, T &&obj);
  // Inserts a KV pair with key [k] and value [obj] into the trie. This method
  // copies [obj] rather than move it as the method above does.
  bool insert(const Key &k, const T &obj) { return insert(k, T(obj)); }

  // Removes the KV pair for key [k] from the trie.
  bool remove(const Key &k);

  // Returns the number of KV pairs in the trie.
  size_t size() const { return size_.load(std::memory_order_relaxed); }

  // Returns true when there are no KV pairs in the trie.
  bool empty() const { return size() == 0; }

  // Removes all KV pairs from the trie.
  void clear()
  {
    free_all_nodes(&root_);
    init_root_node();
    size_ = 0;
  }

  // Executes [func] on each element.
  template<typename F> void for_each(F func)
  {
    HexArray prefix;
    auto f = [&func](const HexArray &path, Node &node, Node * /*parent*/) {
      switch (node.type) {
      case Node::Type::kBranch:
      case Node::Type::kLeaf:
        if (node.value) {
          func({path, *node.value});
        }
        break;
      default:
        break;
      }
    };
    walk<Node>(&root_, prefix, f);
  }

  // Executes [func] in parallel on each element using a WorkerPool.
  template<typename F>
  void for_each_parallel(F /*func*/,
                         size_t /*width*/ = kDefaultParallelRangeWidth,
                         WorkerPool & /*pool*/ = WorkerPool::get_default_pool())
  {
    CHECK(false);
  }

  // If called with true, nodes are hashed on every [insert()/remove()]
  // operation. If called with false, nodes are only hashed when
  // [generate_all_hashes()] is called.
  //
  // Disabling hashing on every op improves performance when reconstructing a
  // trie from disk.
  void set_should_hash_nodes_on_each_op(bool to_set)
  {
    should_hash_nodes_on_each_op_ = to_set;
  }

  // Generates a hash for every node in the tree.
  void generate_all_hashes();
  void generate_hashes_for_levels(int num_levels);

private:
  // Returns a string representation of [node] and all nodes under it in the
  // trie.
  static std::string string_from_node(const Node &node);

  // Recursive helper function for [check()].
  static bool check_helper(Node *root);

  // Finds the node for key [k]. Returns nullptr if [k] is not in the trie.
  Node *find_node(const Key &k);

  // Helper function for [insert()]. When we add a KV pair and there is an
  // extension node below the lowest branch for that pair, we need to handle the
  // extension node and its subtree.
  Node *insert_handle_extension(Node *current_extension, const Key &k,
                                size_t idx, T &&obj);

  // Helper function for [insert()]. When we add a KV pair and there is a leaf
  // node below the lowest branch for that pair, we need to handle the leaf
  // node.
  Node *insert_handle_leaf(Node *current_leaf, const Key &k, size_t idx,
                           T &&obj);

  // Helper function for [remove()]. When we remove a KV pair, the closest
  // branch to it may now have fewer than "two" KV pairs under it. In this case,
  // the branch is no longer needed, so this function removes it.
  Node *remove_branch(const Key &k, Node *branch, size_t num_children);

  // Does a post-order traversal of the trie starting at [root] with path
  // [prefix], calling [f] on each node.
  //
  // A post-order traversal visits all of the children of a node before visiting
  // the node itself. This is useful when regenerating all of the node hashes in
  // a trie or freeing all of the nodes.
  //
  // [NodeType] must either be [Node] or [const Node].
  template<typename NodeType>
  static void
  walk(NodeType *root, const HexArray &prefix,
       const std::function<void(const HexArray &, NodeType &, NodeType *)> &f);

  // Returns the lowest branch in the trie on [path]. [idx] is set to the index
  // of the hex digit in [path] that corresponds to the lowest branch.
  //
  // If a node for [path] already exists in the trie, [existing_key] is set to
  // the node. Thus, since a KV pair can only be placed in a branch or a leaf,
  // [existing_key] is only set to nodes of these types. Regardless of whether
  // this node exist, the function always returns the lowest branch in the trie
  // on [path]. Note that [existing_key] should be a [nullptr] when this
  // function is called so that the caller can determine whether [existing_key]
  // is set to a node.
  static Node *traverse_to_lowest_branch(const HexArray &path, Node *root,
                                         size_t &idx, Node *&existing_key);

  // Computes and writes the hash for [node] only if
  // [should_hash_nodes_on_each_op_] is true.
  void compute_node_hash(Node &node) const;

  // Regenerates the hashes *only* for the nodes on the path from [n] to the
  // root of the trie.
  void generate_hashes_from(Node *n, Node *locked_through) const;

  // Frees all of the nodes starting at [root]. Does not free [root_].
  void free_all_nodes(Node *root);

  // Initializes the root node. The root node is always a branch.
  void init_root_node()
  {
    root_.clear();
    root_.init_branch(/*parent=*/nullptr);
    compute_node_hash(root_);
  }

  // The root node in the trie. The root node always exists and is always a
  // branch.
  Node root_;
  // The number of KV pairs in the trie.
  std::atomic<size_t> size_ = 0;
  // If this is true, nodes are hashed on every [insert()/remove()] operation.
  // If this is false, nodes are only hashed when [generate_all_hashes()] is
  // called.
  bool should_hash_nodes_on_each_op_ = true;

  Arena arena_;
};

} // namespace hogd

// We need to define the formatter in the [std] namespace.
template<>
struct std::formatter<hogd::MerkleTrie> : std::formatter<std::string> {
  auto format(const hogd::MerkleTrie &trie, std::format_context &ctx) const
  {
    std::ostringstream oss;
    oss << trie;
    return std::formatter<std::string>::format(oss.str(), ctx);
  }
};
