// merkle.h - provides a merkle patricia trie

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>

#include <format>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/arena.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/node.h"
#include "types.hh"

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

  MerkleTrie() { init_root_node(); }

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

  // Performs basic sanity checks on the trie.
  bool check() { return check_helper(&root_); }

  // Returns the hash of the root node in the trie.
  SHA256Hash get_root_hash()
  {
    // It is fine to spin on [try_lock_reader()] because we know the root node
    // is never deleted by a writer.
    while (!root_.hash_lock.try_lock_reader()) {
      cpu_relax();
    }

    SHA256Hash ret = root_.hash;
    root_.hash_lock.unlock();
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
                    const std::span<const std::byte> &value);

  // Finds the value hash for key [k]. If key [k] does not exist in the trie,
  // returns a nullptr. If true is returned, you *must* call [find_end(cookie)]
  // when you are done accessing the value hash!
  //
  // We guarantee pointer stability for the value hash.
  bool find_begin(const Key &k, SHA256Hash *&value_hash, void *&cookie);

  // You must call this when you are done accessing a value returned by
  // [find_begin].
  static void find_end(void *cookie);

  // Inserts a KV pair with key [k] and value [obj] into the trie. Fails if the
  // key already exists in the trie. Returns true on success and false on
  // failure.
  bool insert(const Key &k, const std::span<const std::byte> &obj);

  // Inserts a KV pair with key [k] and value [obj] into the trie. If the key
  // already exists, the value in the trie is updated.
  void insert_or_update(const Key &k, const std::span<const std::byte> &obj);

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
  template<typename F>
  void for_each(F func, const HexArray &root_prefix = HexArray())
  {
    auto f = [&func](const HexArray &path, Node &node, Node * /*parent*/) {
      switch (node.type) {
      case Node::Type::kBranch:
      case Node::Type::kLeaf:
        if (node.value_hash) {
          func({path, *node.value_hash});
        }
        break;
      default:
        break;
      }
    };
    walk<Node>(&root_, root_prefix, f);
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

  // Generates a hash for every node in the trie. If the trie is modified while
  // this function runs, the behavior is undefined.
  void generate_all_hashes();

  // We do compute the hashes for the top two levels of nodes in the trie on
  // each operation that modifies the trie. Rather, to avoid rehashing these
  // nodes many times, we only hash them once when this function is called.
  void generate_top_level_hashes();

private:
  // Returns a string representation of [node] and all nodes under it in the
  // trie.
  static std::string string_from_node(const Node &node);

  // Recursive helper function for [check()].
  static bool check_helper(Node *root);

  // Finds the node for key [k]. Returns nullptr if [k] is not in the trie.
  Node *find_node(const Key &k);

  // Inserts key [k] with value [obj]. If [update] is true, the value is updated
  // in the trie if the key already exists. Otherwise, if [update] is false, if
  // the key already exists in the trie then the insert fails.
  //
  // Returns true on success and false on failure.
  bool insert_helper(const Key &k, const std::span<const std::byte> &obj,
                     bool update);

  // Helper function for [insert()]. When we add a KV pair and there is an
  // extension node below the lowest branch for that pair, we need to handle the
  // extension node and its subtree.
  Node *insert_handle_extension(Node *current_extension, const Key &k,
                                size_t idx,
                                const std::span<const std::byte> &obj);

  // Helper function for [insert()]. When we add a KV pair and there is a leaf
  // node below the lowest branch for that pair, we need to handle the leaf
  // node.
  Node *insert_handle_leaf(Node *current_leaf, const Key &k, size_t idx,
                           const std::span<const std::byte> &obj);

  // Returns true if we will need to remove [branch] after removing a KV pair
  // that is "under" it.
  bool will_remove_branch(const Node *branch, size_t num_children);

  // Acquires the locks we need to remove a KV pair. [locked_through] is the
  // topmost node we acquired a lock for. [num_children] is the number of
  // children nodes of [branch]. [remaining_child_hex] is the hex digit of child
  // that will remain (if any) after the remove operation.
  // We do not need to pass [num_children] and [remaining_child_hex] back to the
  // calling function per se because the parent function can compute these
  // itself. However, [remove_grab_locks] needs to compute them in order to
  // figure out which nodes it needs to lock, so we may as well pass them back
  // to the parent to save some work.
  //
  // If this method is unable to acquire all of the locks we need, it returns
  // false. Otherwise it returns true.
  bool remove_grab_locks(Node *branch, Node *existing_key,
                         Node *&locked_through, size_t &num_children,
                         std::byte &remaining_child_hex);

  // Helper function for [remove()]. When we remove a KV pair, the closest
  // branch to it may now have fewer than "two" KV pairs under it. In this case,
  // the branch is no longer needed, so this function removes it.
  Node *remove_branch(Node *branch, size_t num_children,
                      std::byte remaining_child_hex);

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

  friend class MerkleTriePrefix;

  // The root node in the trie. The root node always exists and is always a
  // branch.
  Node root_;
  // The number of KV pairs in the trie.
  std::atomic<size_t> size_ = 0;
  // If this is true, nodes are hashed on every [insert()/remove()] operation.
  // If this is false, nodes are only hashed when [generate_all_hashes()] is
  // called.
  bool should_hash_nodes_on_each_op_ = true;
  // An arena for allocating nodes.
  Arena<Node> arena_;
};

// This is an optimization for the [MerkleTrie]. It exposes the same exact API,
// except empty keys are not allowed.
//
// When using the [MerkleTrie] with multithreading, the root node is under heavy
// contention since all threads must acquire the reader lock on it before they
// can look at any other nodes. This leads to performance degradation.
//
// [MerkleTriePrefix] removes this contention. It creates a root "node", where
// each of the child point to a different [MerkleTrie]. This root "node" is
// never modified, so there is no need to acquire a reader lock to access it.
// Each [MerkleTrie] represents a prefix of one hex digit. For example, the
// [MerkleTrie] that is child [0xa] holds all keys that start with [0xa].
//
// Example: (This is exactly the same as the example for [MerkleTrie] above.)
// MerkleTriePrefix trie;
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
class MerkleTriePrefix {
public:
  MerkleTriePrefix() = default;

  std::pair<SHA256Hash, std::vector<std::byte>>
  get_proof(const MerkleTrie::Key &k)
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
  get_proof_by_node(const MerkleTrie::Key &k)
  {
    if (k.empty()) {
      // We do not support empty keys.
      return {SHA256Hash(), {}};
    }
    const auto &[index, k_no_prefix] = get_prefix(k);

    Node root;
    root.init_branch(/*parent=*/nullptr);
    for (size_t i = 0; i < tries_.size(); i++) {
      root.child[i] = &tries_[i].root_;
      if (i == index) {
        tries_[i].root_.parent = &root;
      }
    }
    std::vector<std::vector<std::byte>> proof =
        tries_[index].get_proof_by_node(k_no_prefix).second;
    tries_[index].root_.parent = nullptr;
    return {root.compute_node_hash(), proof};
  }

  static bool prove(const SHA256Hash &root_hash,
                    const std::vector<std::byte> &encodings,
                    const MerkleTrie::Key &k,
                    const std::span<const std::byte> &value)
  {
    return MerkleTrie::prove(root_hash, encodings, k, value);
  }

  bool find_begin(const MerkleTrie::Key &k, SHA256Hash *&value_hash,
                  void *&cookie)
  {
    if (k.empty()) {
      // We do not support empty keys.
      return false;
    }

    const auto &[index, k_no_prefix] = get_prefix(k);
    return tries_[index].find_begin(k_no_prefix, value_hash, cookie);
  }

  // You must call this when you are done accessing a value returned by
  // [find_begin].
  static void find_end(void *cookie) { MerkleTrie::find_end(cookie); }

  bool insert(const MerkleTrie::Key &k, const std::span<const std::byte> &obj)
  {
    if (k.empty()) {
      // We do not support empty keys.
      return false;
    }

    const auto &[index, k_no_prefix] = get_prefix(k);
    return tries_[index].insert(k_no_prefix, std::move(obj));
  }

  bool remove(const MerkleTrie::Key &k)
  {
    if (k.empty()) {
      // We do not support empty keys.
      return false;
    }

    const auto &[index, k_no_prefix] = get_prefix(k);
    return tries_[index].remove(k_no_prefix);
  }

  void clear()
  {
    for (MerkleTrie &trie : tries_) {
      trie.clear();
    }
  }

  template<typename F> void for_each(F func)
  {
    for (size_t i = 0; i < tries_.size(); i++) {
      HexArray prefix;
      prefix.append(std::byte(i));
      tries_[i].for_each(func, prefix);
    }
  }

  void set_should_hash_nodes_on_each_op(bool to_set)
  {
    for (MerkleTrie &trie : tries_) {
      trie.set_should_hash_nodes_on_each_op(to_set);
    }
  }

  void generate_top_level_hashes()
  {
    for (MerkleTrie &trie : tries_) {
      trie.generate_top_level_hashes();
    }
  }

  void generate_all_hashes()
  {
    for (MerkleTrie &trie : tries_) {
      trie.generate_all_hashes();
    }
  }

  bool check()
  {
    bool ret = true;
    for (MerkleTrie &trie : tries_) {
      ret = trie.check() && ret;
    }
    return ret;
  }

  size_t size()
  {
    size_t count = 0;
    for (MerkleTrie &trie : tries_) {
      count += trie.size();
    }
    return count;
  }

  bool empty() { return size() == 0U; }

private:
  static constexpr size_t kPrefixSize = 1;
  static_assert(kPrefixSize != 0U);

  static constexpr size_t kNumTries = 16;
  static_assert(kNumTries == 1 << (4 * kPrefixSize));

  static std::pair<size_t, HexArray> get_prefix(const MerkleTrie::Key &k)
  {
    CHECK_GT(k.size(), 0UL);

    auto index = static_cast<size_t>(k.get(0));
    MerkleTrie::Key k_no_prefix = k.get_subarray(1, k.size());
    return {index, k_no_prefix};
  }

  std::array<MerkleTrie, kNumTries> tries_;
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
