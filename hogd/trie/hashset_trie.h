#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "hogd/trie/prefix.h"

// blocked on XDR types
// for now, replace
using Hash = std::array<uint8_t, 32>;
// #include "xdr/types.h"

namespace hog {

class HashsetTrieNode {
public:
  struct NodeMetadata {
    Hash hash;
    uint32_t size = 0;
  };

  using prefix_t = ByteArrayPrefix<40>; // 8 bytes for clear threshold, big
                                        // endian, then 32 bytes for hash
  prefix_t prefix_;
  PrefixLenBits prefix_len_;
  std::array<std::atomic<HashsetTrieNode *>, 16> children_;

  HashsetTrieNode(prefix_t const &_prefix, PrefixLenBits const &_prefix_len);
  HashsetTrieNode() : prefix_len_(0) {}

  uint8_t get_branch_bits(prefix_t const &other) const;

  void set_as_non_owner();
  void clear_hash();

  NodeMetadata *get_metadata(this auto &self)
  {
    return self.hash_.load(std::memory_order_relaxed);
  }

  PrefixLenBits get_prefix_match_len(prefix_t const &other) const;

  // Can only be called if child nodes all have metadata already computed
  const NodeMetadata *rehash(std::vector<uint8_t> &workspace);

  static prefix_t make_prefix(uint64_t clear_threshold, Hash const &elt);

  ~HashsetTrieNode();

  // for garbage collecting extraneous nodes
  std::unique_ptr<HashsetTrieNode> maybe_extract_singlechild();

private:
  std::atomic<NodeMetadata *> hash_ = nullptr;
  std::atomic<bool> owns_children_ = true;
};

class HashsetTrie {
public:
  bool insert(uint64_t clear_threshold, Hash const &hash);

  auto remove_clear_threshold(uint64_t clear_threshold)
  {
    return remove_clear_threshold(
        HashsetTrieNode::make_prefix(clear_threshold, Hash()));
  }

  // TODO(geoff): figure out if there's a way to shortcircuit the way back to
  // parent We can tell if current thread was the one to set the current node,
  // but it's unclear how to take advantage.  Mainly, what happens if you
  // shortcircuit but then an earlier level in the recursion needs the value?
  // (and other threads expect it to be responsible for that value?)

  // target is purely a guiding heuristic.  We recurse down towards that prefix
  // first, and then branch out to fill in the rest of the tree.  This hopefully
  // spreads out the work across concurrent threads (since we generally know
  // which prefixes are modified in a current round, but we don't need 100%
  // accuracy on that).
  const HashsetTrieNode::NodeMetadata *
  guided_rehash(HashsetTrieNode::prefix_t const &target)
  {
    std::vector<uint8_t> workspace;
    return guided_rehash(target, workspace);
  }

  const HashsetTrieNode::NodeMetadata *
  guided_rehash(HashsetTrieNode::prefix_t const &target,
                std::vector<uint8_t> &workspace);

private:
  HashsetTrieNode root_;

  // a method that removes all elements below a certain threshold
  std::vector<std::unique_ptr<HashsetTrieNode>>
  remove_clear_threshold(HashsetTrieNode::prefix_t const &clear_threshold);
};

} // namespace hog
