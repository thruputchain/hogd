#pragma once

#include "hogd/trie/hashset_replay_trie_node.h"
#include "hogd/trie/short_allocator.h"

#include "hogd/crypto/sip24.h"

#include <mutex>

namespace hog {

class ReplayTrie {
public:
  class InsertRef {
  public:
    explicit InsertRef(ReplayTrie &base_tree)
      : base_(base_tree), lock_(base_tree.mtx_)
    {}

    bool try_insert(ReplayTrieNode::element_t const &elt)
    {
      return base_.try_insert(elt, base_.get_shorthash(elt));
    }

    // If you call undo_try_insert (i.e. rolling back a tx)
    // you're responsible for calling gc_if_empty on that element later
    void undo_try_insert(ReplayTrieNode::element_t const &elt)
    {
      base_.undo_try_insert(elt, base_.get_shorthash(elt));
    }

  private:
    ReplayTrie &base_;
    std::lock_guard<std::mutex> lock_;
  };

  class RemoveRef {
  public:
    explicit RemoveRef(ReplayTrie &base_tree)
      : base_(base_tree), lock_(base_tree.mtx_)
    {}

    void remove(ReplayTrieNode::element_t const &elt)
    {
      auto hash = base_.get_shorthash(elt);
      // don't check return val, as root is never deleted
      base_.remove(elt, hash, true);
    }

    void gc_if_empty(ReplayTrieNode::element_t const &elt)
    {
      auto hash = base_.get_shorthash(elt);
      // don't check return val, as root is never deleted
      base_.remove(elt, hash, false);
    }

  private:
    ReplayTrie &base_;
    std::lock_guard<std::mutex> lock_;
  };

  explicit ReplayTrie(ShortAllocator<ReplayTrieNode> &allocator)
    : allocator_(allocator), sip24_key_(sip24_keygen())
  {}

  InsertRef open_for_insertions() { return InsertRef(*this); }

  RemoveRef open_for_removals() { return RemoveRef(*this); }

  uint64_t get_shorthash(ReplayTrieNode::element_t const &element) const;

  ~ReplayTrie();

private:
  ReplayTrieNode root_;

  ShortAllocator<ReplayTrieNode> &allocator_;

  std::mutex mtx_;

  Sip24Key sip24_key_;

  // returns true if element is inserted, false if elt was already present
  bool try_insert(ReplayTrieNode::element_t const &element,
                  ReplayTrieNode::hash_t const &hash);

  void undo_try_insert(ReplayTrieNode::element_t const &element,
                       ReplayTrieNode::hash_t const &hash);

  void remove(ReplayTrieNode::element_t const &element,
              ReplayTrieNode::hash_t const &hash, bool element_is_real);
};

} // namespace hog
