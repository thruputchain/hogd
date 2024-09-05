#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>

#include "hogd/trie/short_allocator.h"

#include "hogd/base/non_movable.h"

namespace hog {

// Supports insertions concurrent with insertions,
// and deletions concurrent with deletions,
// but NOT insertions concurrent with deletions.

/**
 * I have spent WAY too long walking through case by case analysis
 * for a variety of different data structures trying to solve this problem.
 * If you can come up with a better alternative, I am all ears.
 * This is the best I can come up with,
 * and has the advantage of (1) being simple to prove correctness
 * and (2) near-minimal memory contention.
 *
 * The main drawback is that the trie is not _normalized_;
 * that is, there might be nodes in the trie with only one child.
 * (along with not supporting concurrent insert/delete).
 *
 * To mitigate this, we use short keys (the output of SipHash-2-4, 64-bits,
 * truncated to 33 -- a multiple of 3, chosen somewhat arbitrarily -- varying
 * just changes the probability of collision, which we handle gracefully), so
 * that an adversary cannot construct an access pattern guaranteed to produce
 * many leftover internal nodes (at least, not that I am aware of how to do),
 * and to limit the maximum  number of internal nodes on any path (11).
 */

/**
 * No need to even worry about anything ABA problem related
 * simply because inserts only allocate new nodes (or delete nodes that don't
 * make it into the shared view) and removes only remove nodes, with no new
 * allocations. This is why I removed the deferred_free interface to the short
 * allocator.
 */

uint64_t get_shorthash(std::array<uint64_t, 4> const &elt);

struct alignas(64) ReplayTrieNode : public NonMovableOrCopyable {

  constexpr static uint16_t kMaxHashLen = 33;
  constexpr static uint64_t kHashMask =
      (static_cast<uint64_t>(1) << kMaxHashLen) - 1;

  using element_t = std::array<uint64_t, 4>;
  using hash_t = uint64_t; // INVARIANT: extra top bits are 0
  using allocator_t = ShortAllocator<ReplayTrieNode>;
  using children_t = std::array<std::atomic<uint32_t>, 8>;

  struct LeafCollisionHandler {
    std::set<element_t> elts;
    std::mutex mtx;

    explicit LeafCollisionHandler(element_t const &elt) { elts.insert(elt); }

    bool try_insert(element_t const &elt);
    bool remove(element_t const &elt);
    bool is_empty();
  };

  static_assert(sizeof(children_t) == 32, "mismatch");

  static_assert(__cpp_lib_atomic_value_initialization >= 201911L,
                "invalid atomic init otherwise");

  children_t children;
  const hash_t prefix;
  const uint16_t prefix_len = 0;
  std::atomic<uint8_t> num_active_children = 0;
  std::atomic<LeafCollisionHandler *> element = nullptr;

  uint16_t get_prefix_match_len(hash_t const &hash) const;
  uint8_t get_branch_bits(hash_t const &hash) const;

  uint32_t get_child(uint8_t branch_bits);
  bool try_atomic_swap(uint8_t branch_bits, uint32_t &expect, uint32_t desire);

  void set_child(uint8_t branch_bits, uint32_t child);

  struct LeafCtorT {};
  struct MidlevelCtorT {};

  // leaf constructor
  ReplayTrieNode(element_t const &new_element, hash_t const &hash, LeafCtorT);

  // midlevel ctor
  ReplayTrieNode(hash_t const &hash, uint8_t prefix_len, MidlevelCtorT);

  // root ctor
  ReplayTrieNode() : prefix() {}

  // returns true if node should be deleted
  bool remove(element_t const &elt, hash_t const &hash, allocator_t &allocator,
              bool element_is_real);

  // dtor doesn't have allocator, and this shouldn't run if node doesn't own
  // children
  void free_children(allocator_t &allocator);

  ~ReplayTrieNode();
};

static_assert(sizeof(ReplayTrieNode) <= 64, "too big for cache line");

} // namespace hog
