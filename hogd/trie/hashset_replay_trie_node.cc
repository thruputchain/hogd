#include "hogd/trie/hashset_replay_trie_node.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <stdexcept>

namespace hog {

uint16_t
ReplayTrieNode::get_prefix_match_len(hash_t const &hash) const
{
  uint64_t diff = hash ^ prefix;
  if (diff == 0) {
    return prefix_len;
  }
  uint16_t out = std::min<uint16_t>(prefix_len, std::countr_zero(diff));
  out -= (out % 3);

  return out;
}

uint8_t
ReplayTrieNode::get_branch_bits(hash_t const &hash) const
{
  assert(prefix_len != kMaxHashLen);
  return (hash >> prefix_len) & 7;
}

// leaf ctor
ReplayTrieNode::ReplayTrieNode(element_t const &new_element, hash_t const &hash,
                               LeafCtorT)
  : prefix(hash), prefix_len(kMaxHashLen),
    element(new LeafCollisionHandler(new_element))
{}

// midlevel ctor
ReplayTrieNode::ReplayTrieNode(hash_t const &hash, uint8_t prefix_len,
                               MidlevelCtorT)
  : prefix(hash), // don't need to bother truncating!
    prefix_len(prefix_len)
{}

uint32_t
ReplayTrieNode::get_child(uint8_t branch_bits)
{
  return children[branch_bits].load(std::memory_order_relaxed);
}

void
ReplayTrieNode::set_child(uint8_t branch_bits, uint32_t child)
{
  uint32_t prev_child = get_child(branch_bits);
  if (!try_atomic_swap(branch_bits, prev_child, child)) {
    throw std::runtime_error("impossible");
  }
}

bool
ReplayTrieNode::try_atomic_swap(uint8_t branch_bits, uint32_t &expect,
                                uint32_t desire)
{
  bool expect_null = (expect == 0);
  bool desire_null = (desire == 0);

  bool res =
      children[branch_bits].compare_exchange_strong(expect, desire,
                                                    std::memory_order_relaxed);

  if (res) {
    if (expect_null && (!desire_null)) {
      num_active_children.fetch_add(1, std::memory_order_relaxed);
    }
  }

  return res;
}

bool
ReplayTrieNode::LeafCollisionHandler::try_insert(element_t const &elt)
{
  std::lock_guard lock(mtx);
  if (elts.contains(elt)) {
    return false;
  }
  elts.insert(elt);
  return true;
}

bool
ReplayTrieNode::LeafCollisionHandler::remove(element_t const &elt)
{
  std::lock_guard lock(mtx);
  [[maybe_unused]] auto n_elts_removed = elts.erase(elt);
  assert(n_elts_removed == 1);
  return elts.empty();
}

bool
ReplayTrieNode::LeafCollisionHandler::is_empty()
{
  std::lock_guard lock(mtx);
  return elts.empty();
}

// This is just very ugly when written
// as a non-recursive version, and this should never be on a hot path
// NOLINTBEGIN(misc-no-recursion)
void
ReplayTrieNode::free_children(allocator_t &allocator)
{
  for (auto i = 0U; i < children.size(); i++) {
    uint32_t child = get_child(i);
    if (child != 0) {
      allocator.get(child)->free_children(allocator);
      allocator.get(child)->~ReplayTrieNode();
      allocator.free(child);
    }
  }
}
// NOLINTEND(misc-no-recursion)

ReplayTrieNode::~ReplayTrieNode()
{
  delete element.load(std::memory_order_relaxed);
}

/*

bool
SimpleReplayTreeNode::remove(element_t const &elt, hash_t const &hash,
                             allocator_t &allocator, bool element_is_real)
{
  if (prefix_len == MAX_HASH_LEN) {
    if (element_is_real) {
      return element.load(std::memory_order_relaxed)->remove(elt);
    }
    else {
      return element.load(std::memory_order_relaxed)->is_empty();
    }
  }

  uint8_t bb = get_branch_bits(hash);

  uint32_t child = get_child(bb);

  if (child == 0) {
    throw std::runtime_error("impossible");
  }

  auto *child_ptr = allocator.get(child);

  auto deletion_res = child_ptr->remove(elt, hash, allocator, element_is_real);

  if (deletion_res) {
    uint8_t prev_num_active_children =
        num_active_children.fetch_sub(1, std::memory_order_relaxed);

    if (!try_atomic_swap(bb, child, 0)) {
      throw std::runtime_error("impossible: only one thread can reach 0 active "
                               "children during deletion");
    }

    child_ptr->~SimpleReplayTreeNode();
    allocator.free(child);

    if (prev_num_active_children == 1) {
      return true;
    }
  }
  return false;
}  */

} // namespace hog
