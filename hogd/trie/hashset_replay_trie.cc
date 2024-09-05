#include "hogd/trie/hashset_replay_trie.h"

#include "hogd/crypto/sip24.h"
#include "hogd/trie/hashset_replay_trie_node.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <exception>
#include <vector>

namespace hog {

ReplayTrie::~ReplayTrie()
{
  root_.free_children(allocator_);
};

uint64_t
ReplayTrie::get_shorthash(ReplayTrieNode::element_t const &element) const
{
  return sip24(reinterpret_cast<const uint8_t *>(element.data()), 32,
               sip24_key_) &
         ReplayTrieNode::kHashMask;
}

bool
ReplayTrie::try_insert(ReplayTrieNode::element_t const &element,
                       ReplayTrieNode::hash_t const &hash)
{
  auto *visitor = &root_;

  uint32_t new_child = 0;

  while (true) {
    if (visitor->prefix_len == ReplayTrieNode::kMaxHashLen) {
      assert(hash == visitor->prefix);
      return visitor->element.load(std::memory_order_relaxed)
          ->try_insert(element);
    }

    assert(visitor->get_prefix_match_len(hash) == visitor->prefix_len);

    auto bb = visitor->get_branch_bits(hash);
    uint32_t child = visitor->get_child(bb);

    while (true) {

      auto *child_ptr = allocator_.get(child);

      if (child_ptr == nullptr) {
        if (new_child == 0) {
          new_child = allocator_.allocate();
        }

        new (allocator_.get(new_child))
            ReplayTrieNode(element, hash, ReplayTrieNode::LeafCtorT{});

        if (visitor->try_atomic_swap(bb, child, new_child)) {
          // new_child now owned by child
          // If we weren't returning immediately, we would
          // need to set new_child = 0
          return true;
        }
        allocator_.get(new_child)->~ReplayTrieNode();

        // SPINLOCK_PAUSE();
        continue;
      }

      auto child_match_len = child_ptr->get_prefix_match_len(hash);

      if (child_match_len == child_ptr->prefix_len) {
        visitor = child_ptr;
        break; // to outer loop
      }

      if (new_child == 0) {
        new_child = allocator_.allocate();
      }

      auto *new_child_ptr = new (allocator_.get(new_child))
          ReplayTrieNode(hash, child_match_len,
                         ReplayTrieNode::MidlevelCtorT{});

      new_child_ptr->set_child(new_child_ptr->get_branch_bits(
                                   child_ptr->prefix),
                               child);

      if (visitor->try_atomic_swap(bb, child, new_child)) {
        new_child = 0;
        visitor = new_child_ptr;
        break; // to outer loop
      }

      new_child_ptr->~ReplayTrieNode();
      // SPINLOCK_PAUSE();
    }
  }
}

void
ReplayTrie::undo_try_insert(ReplayTrieNode::element_t const &element,
                            ReplayTrieNode::hash_t const &hash)
{
  auto *visitor = &root_;

  while (true) {

    if (visitor->prefix_len == ReplayTrieNode::kMaxHashLen) {
      [[maybe_unused]] auto res =
          visitor->element.load(std::memory_order_relaxed)->remove(element);
      assert(res);
      return;
    }

    auto bb = visitor->get_branch_bits(hash);

    auto child = visitor->get_child(bb);

    assert(child != 0);

    visitor = allocator_.get(child);
    if (!visitor) {
      std::terminate(); // TODO(geoff) error handling here, once errbase merged
    }
  }
}

void
ReplayTrie::remove(ReplayTrieNode::element_t const &element,
                   ReplayTrieNode::hash_t const &hash, bool element_is_real)
{
  struct StackT {
    ReplayTrieNode *parent;
    uint8_t bb;
    uint32_t child_ptr;
  };

  std::vector<StackT> stack;

  stack.push_back(StackT{.parent = &root_, .bb = 0, .child_ptr = 0});

  bool did_recent_deletion = false;

  while (true) {
    auto &back = stack.back();

    assert(back.parent);

    if (back.parent->prefix_len == ReplayTrieNode::kMaxHashLen) {
      if (element_is_real) {
        did_recent_deletion =
            back.parent->element.load(std::memory_order_relaxed)
                ->remove(element);
      }
      else {
        did_recent_deletion =
            back.parent->element.load(std::memory_order_relaxed)->is_empty();
      }
      stack.pop_back(); // drop the last (not-fully-initialized) elt on stack
      break;
    }

    back.bb = back.parent->get_branch_bits(hash);

    uint32_t child_ptr = back.parent->get_child(back.bb);

    back.child_ptr = child_ptr;

    stack.push_back(
        StackT{.parent = allocator_.get(child_ptr), .bb = 0, .child_ptr = 0});
  }

  while (!stack.empty()) {
    if (!did_recent_deletion)
      return;

    auto &back = stack.back();

    uint8_t prev_num_active_children =
        back.parent->num_active_children.fetch_sub(1,
                                                   std::memory_order_relaxed);

    [[maybe_unused]] auto res =
        back.parent->try_atomic_swap(back.bb, back.child_ptr, 0);
    assert(res); // only one thread can delete a child, by construction

    allocator_.get(back.child_ptr)->~ReplayTrieNode();
    allocator_.free(back.child_ptr);

    if (prev_num_active_children != 1) {
      did_recent_deletion = false;
    }
    stack.pop_back();
  }
}

} // namespace hog
