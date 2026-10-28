// merkle.h - provides a merkle patricia trie

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <stack>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/merkle/allocator.h"
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
// trie.generate_all_hashes();
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

  // Mutating operations keep leaf hashes current. Branch and extension hashes
  // are deferred: call [generate_all_hashes()] after finishing mutations before
  // reading the root hash or generating proofs.
  explicit MerkleTrie(
      Allocator<Node>::Type allocator_type = Allocator<Node>::Type::kDefault)
    : allocator_(allocator_type)
  {
    init_root_node();
  }

  ~MerkleTrie() // NOLINT(bugprone-exception-escape)
  {
    if (allocator_.type() != Allocator<Node>::Type::kBump) {
      free_all_nodes(&root_);
    }
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
  bool check() { return check_helper(&root_, root_prefix_); }

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
  // function above returns the encodings concatenated together.
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

  // Finds the key/value commitment for [k], as computed by
  // [Node::compute_value_commitment]. Returns false if the key is absent.
  // On success, call [find_end(cookie)] when done accessing [value_hash].
  // We guarantee pointer stability for the stored commitment.
  bool find_begin(const Key &k, SHA256Hash *&value_hash, void *&cookie);

  // You must call this when you are done accessing a value returned by
  // [find_begin].
  static void find_end(void *cookie);

  // Inserts a KV pair with key [k] and value [obj] into the trie. Fails if the
  // key already exists in the trie. Returns true on success and false on
  // failure.
  bool insert(const Key &k, const std::span<const std::byte> &obj);

  // Inserts a KV pair with key [k] and value [obj] into the trie. If the key
  // already exists, the value in the trie is updated. Returns true on success
  // and false on failure.
  bool insert_or_update(const Key &k, const std::span<const std::byte> &obj);

  // Removes the KV pair for key [k] from the trie.
  bool remove(const Key &k);

  // Returns the number of KV pairs in the trie.
  size_t size() const { return size_.load(std::memory_order_relaxed); }

  // Returns true when there are no KV pairs in the trie.
  bool empty() const { return size() == 0; }

  // Removes all KV pairs from the trie.
  void clear()
  {
    if (allocator_.type() == Allocator<Node>::Type::kBump) {
      allocator_.clear_all_regions();
    }
    else {
      free_all_nodes(&root_);
    }
    init_root_node();
    size_ = 0;
  }

  // Calls [func] with each complete key and its stored key/value commitment.
  template<typename F> void for_each(F func)
  {
    auto f = [&func](const HexArray &path, Node &node, Node * /*parent*/) {
      switch (node.type) {
      case Node::Type::kBranch:
        if (node.value_hash()) {
          func({path, *node.value_hash()});
        }
        break;
      case Node::Type::kLeaf:
        if (node.value_hash()) {
          func({node.path(), *node.value_hash()});
        }
        break;
      default:
        break;
      }
    };
    walk<Node>(&root_, root_prefix_, f);
  }

  // Refreshes branch and extension hashes; leaf hashes are prepared on insert.
  // If the trie is modified while this function runs, the behavior is
  // undefined.
  void generate_all_hashes(size_t num_threads = available_parallelism());

private:
  // Returns a string representation of [node] and all nodes under it in the
  // trie.
  static std::string string_from_node(const Node &node);

  // Recursive helper function for [check()].
  static bool check_helper(Node *root, const HexArray &prefix);

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
                                size_t idx, Node *new_leaf);

  // Helper function for [insert()]. When we add a KV pair and there is a leaf
  // node below the lowest branch for that pair, we need to handle the leaf
  // node.
  Node *insert_handle_leaf(Node *current_leaf, const Key &k, size_t idx,
                           Node *new_leaf);

  // Returns true if we will need to remove [branch] after removing a KV pair
  // that is "under" it.
  bool will_remove_branch(const Node *branch, size_t num_children);

  // Acquires the locks we need to remove a KV pair. [locked_through] is the
  // topmost node we acquired a lock for. [num_children] is the number of
  // children nodes of [branch]. [remaining_child_hex] is the hex digit of child
  // that will remain (if any) after the remove operation.
  // [locked_through_depth] is the depth of the topmost node that is locked
  // (i.e., the node with the lowest depth that is locked).
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
                         std::byte &remaining_child_hex,
                         size_t &locked_through_depth);

  // Helper function for [remove()]. When we remove a KV pair, the closest
  // branch to it may now have fewer than "two" KV pairs under it. In this case,
  // the branch is no longer needed, so this function removes it.
  Node *remove_branch(Node *branch, size_t num_children,
                      std::byte remaining_child_hex, const Key &branch_key);

  // Does a post-order traversal of the trie starting at [root] with path
  // [prefix], calling [f] on each node. Does not traverse nodes with a depth
  // larger than [max_depth]. If [max_depth] is [std::nullopt], then this method
  // traverses the entire trie.
  //
  // A post-order traversal visits all of the children of a node before visiting
  // the node itself. This is useful when regenerating all of the node hashes in
  // a trie or freeing all of the nodes.
  //
  // [NodeType] must either be [Node] or [const Node].
  template<typename T, bool Enabled>
  using Maybe = std::conditional_t<Enabled, T, std::monostate>;

  template<typename NodeType, bool TrackPath, typename Fn>
  static void walk_impl(NodeType *root, const HexArray &prefix, Fn &&f,
                        std::optional<size_t> max_depth = std::nullopt)
  {
    static_assert(std::is_same_v<NodeType, Node> ||
                      std::is_same_v<NodeType, const Node>,
                  "[NodeType] must be [Node] or [const Node].");

    // We perform a post-order traversal iteratively. We use iteration rather
    // than recursion to avoid exceeding the stack size when traversing deep
    // tries.

    // We use this type along with the data structures below to track where we
    // are in the trie.
    struct CheckInfo {
      // The node we are currently looking at.
      NodeType *node;
      // The parent of this node.
      NodeType *parent;
      // The path that maps to this node when the caller needs it.
      Maybe<HexArray, TrackPath> path;
      // This is only relevant for branch and extension nodes. When we first
      // encounter these nodes in an iterative loop, we cannot *immediately*
      // "visit" them (i.e., execute [f] on them)--we need to visit all of their
      // children first.
      //
      // A branch or extension node will be left in the stack until all of its
      // children have been visited. When this node pushes one of its children
      // onto the stack, the node waits until the child is popped off the stack
      // before pushing the next child. Thus, the node will not enqueue the next
      // child until a subsequent iteration, and thus we need to track this
      // child so that the node knows which child to push at that point.
      //
      // We use [next_child_to_visit] to track which child should be pushed next
      // for [node].
      size_t next_child_to_visit;
      size_t depth;
    };

    // Each step down the trie consumes at least one hex digit of the key (a
    // branch consumes one, and an extension's path is never empty), so the
    // stack holds at most one entry per hex digit plus one for [root]. We
    // reserve that up front so that the traversal never allocates once it has
    // started calling [f]. [free_all_nodes] relies on this: an allocation
    // failure after [f] has freed some nodes would unwind with the trie still
    // pointing at them.
    constexpr size_t kMaxStackSize = HexArray::kMaxNumHexDigits + 1;
    std::vector<CheckInfo> stack_storage;
    stack_storage.reserve(kMaxStackSize);
    std::stack<CheckInfo, std::vector<CheckInfo>> path(
        std::move(stack_storage));
    // Push the root node onto the stack.
    CheckInfo root_info{.node = root,
                        .parent = nullptr,
                        .next_child_to_visit = 0,
                        .depth = 0};
    if constexpr (TrackPath) {
      root_info.path = prefix;
    }
    path.push(root_info);

    while (!path.empty()) {
      CheckInfo &info = path.top();

      if (!info.node) {
        path.pop();
        continue;
      }

      switch (info.node->type) {
      case Node::Type::kBranch:
        CHECK_LE(info.next_child_to_visit, info.node->branch_children().size());
        // Empty slots have no node to visit. Avoid pushing a traversal frame
        // that the null-node check above would immediately pop.
        while (info.next_child_to_visit < info.node->branch_children().size() &&
               !info.node->branch_children()[info.next_child_to_visit]) {
          info.next_child_to_visit++;
        }
        if (info.next_child_to_visit == info.node->branch_children().size() ||
            (max_depth && info.depth == *max_depth)) {
          // We have visited all of the branch's children, so now we can "visit"
          // (i.e., call [f] on) the branch. Or, we have reached the maximum
          // depth we want to visit of the trie, so we now visit the extension
          // node because we do not want to visit its children.
          if constexpr (TrackPath) {
            f(info.path, *info.node, info.parent);
          }
          else {
            f(*info.node, info.parent);
          }
        }
        else {
          // Visit the next child.
          CheckInfo child{
              .node = info.node->branch_children()[info.next_child_to_visit],
              .parent = info.node,
              .next_child_to_visit = 0,
              .depth = info.depth + 1};
          if constexpr (TrackPath) {
            // Append the branch path we are taking to [info.path] so that it
            // holds the path to the child. For example, if
            // [info.next_child_to_visit] is 0x3, then we know we will next
            // visit the child at index 0x3, so we append 0x3 to [info.path].
            info.path.append(std::byte(info.next_child_to_visit));
            child.path = info.path;
            // Now change [info.path] back to holding the path to the branch
            // node.
            info.path.remove_from_back(1);
          }
          // Update [info] before pushing: [path] is backed by a vector, so a
          // push may invalidate [info].
          info.next_child_to_visit++;
          CHECK_LT(path.size(), kMaxStackSize);
          path.push(child);
          continue;
        }
        break;
      case Node::Type::kExtension:
        // An extension node always has exactly one child. It is not possible
        // for an extension node to have no children.
        CHECK_LE(info.next_child_to_visit, 1UL);
        if (info.next_child_to_visit == 1 ||
            (max_depth && info.depth == *max_depth)) {
          // We have visited the extension node's one child, so now we can visit
          // the extension node. Or, we have reached the maximum depth we want
          // to visit of the trie, so we now visit the extension node because we
          // do not want to visit its children.
          if constexpr (TrackPath) {
            f(info.path, *info.node, info.parent);
          }
          else {
            f(*info.node, info.parent);
          }
        }
        else {
          CheckInfo child{.node = info.node->extension_child(),
                          .parent = info.node,
                          .next_child_to_visit = 0,
                          .depth = info.depth + 1};
          if constexpr (TrackPath) {
            // Append the extension node's path to [info.path] so that
            // [info.path] holds the path to the child when we push the child.
            info.path.append(info.node->path());
            child.path = info.path;
            // Now change [info.path] back to holding the path to the extension
            // node.
            info.path.remove_from_back(info.node->path().size());
          }
          // Update [info] before pushing, as above.
          info.next_child_to_visit++;
          CHECK_LT(path.size(), kMaxStackSize);
          path.push(child);
          continue;
        }
        break;
      case Node::Type::kLeaf:
        // A leaf node has no children, so we can visit a leaf immediately upon
        // encountering it.
        CHECK_EQ(info.next_child_to_visit, 0UL);
        if (max_depth) {
          CHECK_LE(info.depth, *max_depth);
        }

        NodeType &n = *info.node;
        if constexpr (TrackPath) {
          // [info.path] is the routing path to the leaf. The leaf itself
          // stores its complete key, which must start with that path.
          f(info.path, n, info.parent);
        }
        else {
          f(n, info.parent);
        }
        break;
      }

      path.pop();
    }
  }

  template<typename NodeType, typename Fn>
  static void walk(NodeType *root, const HexArray &prefix, Fn &&f,
                   std::optional<size_t> max_depth = std::nullopt)
  {
    walk_impl<NodeType, /*TrackPath=*/true>(root, prefix, std::forward<Fn>(f),
                                            max_depth);
  }

  template<typename NodeType, typename Fn>
  static void walk(NodeType *root, Fn &&f,
                   std::optional<size_t> max_depth = std::nullopt)
  {
    walk_impl<NodeType, /*TrackPath=*/false>(root, HexArray(),
                                             std::forward<Fn>(f), max_depth);
  }

  // Returns the lowest branch in the trie on [path]. [idx] is set to the index
  // of the hex digit in [path] that corresponds to the lowest branch.
  // [branch_depth] is the depth of the returned branch.
  //
  // If a node for [path] already exists in the trie, [existing_key] is set to
  // the node. Thus, since a KV pair can only be placed in a branch or a leaf,
  // [existing_key] is only set to nodes of these types. Regardless of whether
  // this node exists, the function always returns the lowest branch in the trie
  // on [path]. Note that [existing_key] should be a [nullptr] when this
  // function is called so that the caller can determine whether [existing_key]
  // is set to a node.
  static Node *traverse_to_lowest_branch(const HexArray &path, Node *root,
                                         size_t &idx, Node *&existing_key,
                                         size_t &branch_depth);

  // Unlocks [n] and the nodes above it up through and including
  // [locked_through].
  void unlock_nodes_from(Node *n, Node *locked_through);

  // Frees all of the nodes starting at [root]. Does not free [root_].
  void free_all_nodes(Node *root);

  // Computes hashes for the entire subtree rooted at [root].
  static void generate_all_hashes_serial(Node *root);

  // Collects disjoint subtrees to hash in parallel.
  static std::vector<Node *> collect_hash_frontier(Node *root,
                                                   size_t max_subtrees);

  // Computes hashes only for nodes above the already-hashed frontier.
  static void generate_all_hashes_above_frontier(
      Node *root, const std::unordered_set<Node *> &frontier);

  // Initializes the root node. The root node is always a branch.
  void init_root_node()
  {
    root_.clear();
    root_.init_branch(/*parent=*/nullptr);
  }

  friend class MerkleTriePrefix;

  // The root node in the trie. The root node always exists and is always a
  // branch.
  Node root_;
  // Routing prefix already represented above root_ (one digit for each inner
  // MerkleTriePrefix trie). Leaves still retain the complete caller key.
  HexArray root_prefix_;
  // The memory allocator for nodes.
  Allocator<Node> allocator_;
  // The number of KV pairs in the trie.
  std::atomic<size_t> size_ = 0;
};

// This is an optimization for the [MerkleTrie]. It exposes the same exact API,
// except empty keys are not allowed.
//
// When using the [MerkleTrie] with multithreading, the root node is under heavy
// contention since all threads must acquire the reader lock on it before they
// can look at any other nodes. This leads to performance degradation.
//
// [MerkleTriePrefix] removes this contention. It creates a fake root "node",
// where each of the children of the fake root "node" point to a different
// [MerkleTrie]. This root "node" is never modified, so there is no need to
// acquire a reader lock to access it. Each [MerkleTrie] represents a prefix of
// one hex digit. For example, the [MerkleTrie] that is child [0xa] of the fake
// root "node" holds all keys that start with [0xa].
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

// NOLINTBEGIN(bugprone-unchecked-optional-access)
class MerkleTriePrefix {
public:
  explicit MerkleTriePrefix(
      Allocator<Node>::Type allocator_type = Allocator<Node>::Type::kDefault)
  {
    for (size_t i = 0; i < tries_.size(); ++i) {
      tries_[i].emplace(allocator_type);
      tries_[i]->root_prefix_.append(std::byte(i));
    }
  }

  ~MerkleTriePrefix()
  {
    if (tries_[0]->allocator_.type() != Allocator<Node>::Type::kBump) {
      try {
        // JobScope drains all reset() jobs before tries_ is destroyed, on
        // every path -- including a submit() failure partway through the loop.
        WorkerPool::JobScope scope;
        scope.reserve(tries_.size());
        for (std::optional<MerkleTrie> &trie : tries_) {
          scope.submit([&trie] { trie.reset(); });
        }
        scope.wait();
      } catch (...) {
        // Unexpected exception thrown.
        CHECK(false);
      }
    }
  }

  SHA256Hash get_root_hash()
  {
    Node root;
    root.init_branch(/*parent=*/nullptr);
    for (size_t i = 0; i < tries_.size(); i++) {
      root.branch_children()[i] = &tries_[i]->root_;
    }
    return root.compute_node_hash();
  }

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
    const size_t index = get_prefix(k);

    Node root;
    root.init_branch(/*parent=*/nullptr);
    for (size_t i = 0; i < tries_.size(); i++) {
      root.branch_children()[i] = &tries_[i]->root_;
      if (i == index) {
        tries_[i]->root_.parent = &root;
      }
    }
    std::vector<std::vector<std::byte>> proof =
        tries_[index]->get_proof_by_node(k).second;
    tries_[index]->root_.parent = nullptr;
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

    const size_t index = get_prefix(k);
    return tries_[index]->find_begin(k, value_hash, cookie);
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

    const size_t index = get_prefix(k);
    return tries_[index]->insert(k, obj);
  }

  bool insert_or_update(const MerkleTrie::Key &k,
                        const std::span<const std::byte> &obj)
  {
    if (k.empty()) {
      // We do not support empty keys.
      return false;
    }

    const size_t index = get_prefix(k);
    return tries_[index]->insert_or_update(k, obj);
  }

  bool remove(const MerkleTrie::Key &k)
  {
    if (k.empty()) {
      // We do not support empty keys.
      return false;
    }

    const size_t index = get_prefix(k);
    return tries_[index]->remove(k);
  }

  void clear()
  {
    for (std::optional<MerkleTrie> &trie : tries_) {
      trie->clear();
    }
  }

  template<typename F> void for_each(F func)
  {
    for (std::optional<MerkleTrie> &trie : tries_) {
      trie->for_each(func);
    }
  }

  void generate_all_hashes(size_t num_threads = available_parallelism())
  {
    num_threads = std::max<size_t>(1, num_threads);
    if (num_threads == 1) {
      for (std::optional<MerkleTrie> &trie : tries_) {
        trie->generate_all_hashes(/*num_threads=*/1);
      }
      return;
    }

    const std::vector<ChunkRange> thread_allocations =
        make_chunks(std::max(num_threads, tries_.size()), tries_.size());
    std::array<std::vector<Node *>, 16> frontiers;

    for (size_t i = 0; i < tries_.size(); i++) {
      std::optional<MerkleTrie> &trie = tries_[i];
      const ChunkRange &allocation = thread_allocations[i];
      const size_t child_threads = allocation.end - allocation.start;
      frontiers[i] =
          MerkleTrie::collect_hash_frontier(&trie->root_, child_threads);
    }

    size_t total_jobs = 0;
    for (size_t i = 0; i < tries_.size(); i++) {
      total_jobs += frontiers[i].size();
    }

    // JobScope joins every worker before the frontier subtree roots (owned by
    // this->tries_) can be touched again, even if a submit() fails mid-loop.
    WorkerPool::JobScope scope;
    scope.reserve(total_jobs);
    for (size_t i = 0; i < tries_.size(); i++) {
      for (Node *subtree_root : frontiers[i]) {
        scope.submit([subtree_root] {
          MerkleTrie::generate_all_hashes_serial(subtree_root);
        });
      }
    }
    scope.wait();

    for (size_t i = 0; i < tries_.size(); i++) {
      std::unordered_set<Node *> frontier(frontiers[i].begin(),
                                          frontiers[i].end());
      MerkleTrie::generate_all_hashes_above_frontier(&tries_[i]->root_,
                                                     frontier);
    }
  }

  bool check()
  {
    bool ret = true;
    for (std::optional<MerkleTrie> &trie : tries_) {
      ret = trie->check() && ret;
    }
    return ret;
  }

  size_t size()
  {
    size_t count = 0;
    for (std::optional<MerkleTrie> &trie : tries_) {
      count += trie->size();
    }
    return count;
  }

  bool empty() { return size() == 0U; }

private:
  static constexpr size_t kPrefixSize = 1;
  static_assert(kPrefixSize != 0U);

  static constexpr size_t kNumTries = 16;
  static_assert(kNumTries == 1 << (4 * kPrefixSize));

  static size_t get_prefix(const MerkleTrie::Key &k)
  {
    CHECK_GT(k.size(), 0UL);

    return static_cast<size_t>(k.get(0));
  }

  std::array<std::optional<MerkleTrie>, kNumTries> tries_;
};
// NOLINTEND(bugprone-unchecked-optional-access)

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
