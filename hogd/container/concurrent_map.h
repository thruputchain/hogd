#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <functional>
#include <span>
#include <variant>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"

namespace hogd {

// A lockfree map designed for heavily contended accesses.
// This map does not support deletion, and does not garbage collect internal
// state. Nor does it rebalance the map, if nodes are inserted in a skewed
// distribution. This means that this map is best used for e.g. one block, and
// then discarded entirely.
//
// The tradeoff is that insertions are lockfree, and that writes are only
// performed if the key is not present. Looking up an entry already in the map
// only perfoms atomic read operations (i.e. no shared locks).
//
// Keys are 32-bit integers.

namespace detail {

// Linked list of values, to resolve hash collisions.
// These are kept in an unsorted (arbitrary) order.  Items are appended to the
// end.
template<typename Value> class ValueBucket {
public:
  template<typename... Args> requires std::constructible_from<Value, Args...>
  ValueBucket(std::span<const uint8_t> key, Args &&...args)
    : key_(key.begin(), key.end()), value_(std::forward<Args>(args)...)
  {}

  ValueBucket(const ValueBucket &) = delete;
  ValueBucket &operator=(const ValueBucket &) = delete;
  ValueBucket(ValueBucket &&) noexcept = delete;
  ValueBucket &operator=(ValueBucket &&) noexcept = delete;

  ~ValueBucket() { delete next_ptr_.load(); }

  const std::vector<uint8_t> key_;
  Value value_;
  std::atomic<ValueBucket<Value> *> next_ptr_;

  static bool key_matches(std::span<const uint8_t> key1,
                          std::span<const uint8_t> key2)
  {
    return key1.size() == key2.size() &&
           std::memcmp(key1.data(), key2.data(), key1.size()) == 0;
  }

  template<typename... Args> requires std::constructible_from<Value, Args...>
  static Value *insert(std::atomic<ValueBucket<Value> *> *scan_ptr,
                       std::span<const uint8_t> key, Args &&...args)
  {
    // The value bucket that we might create in this iteration of insert.
    // Starts as nullptr, instantiated if we try to insert.
    ValueBucket<Value> *new_value_bucket = nullptr;

    ValueBucket<Value> *next_value_bucket =
        scan_ptr->load(std::memory_order_relaxed);

    while (true) {
      // If we're at the end of the list, try to insert.
      if (next_value_bucket == nullptr) {

        if (!new_value_bucket) {
          new_value_bucket =
              new ValueBucket<Value>{key, Value(std::forward<Args>(args)...)};
        }
        assert(key_matches(std::span(new_value_bucket->key_), key));

        if (scan_ptr->compare_exchange_strong(next_value_bucket,
                                              new_value_bucket,
                                              std::memory_order_release,
                                              std::memory_order_relaxed)) {
          return &new_value_bucket->value_;
        }

        // Insertion failed; next_value_bucket now contains the value of
        // scan_ptr.load() from the compare_exchange_strong.
        cpu_relax();
        continue;
      }

      assert(next_value_bucket != nullptr);

      // Not at a nullptr, so try scanning the current object for a match
      if (key_matches(std::span(next_value_bucket->key_), key)) {
        delete new_value_bucket;
        return &next_value_bucket->value_;
      }

      // Continue scanning the list
      scan_ptr = &next_value_bucket->next_ptr_;
      next_value_bucket = scan_ptr->load(std::memory_order_relaxed);
    }
  }
};

template<typename Value> class Node {
public:
  // NOLINTBEGIN(readability-identifier-naming)
  struct root_t {};
  struct leaf_t {};
  struct branch_t {};
  // NOLINTEND(readability-identifier-naming)

  Node(root_t /*unused*/)
    : content_(std::in_place_type<std::array<std::atomic<Node<Value> *>, 16>>),
      key_{0}, key_len_{0}, owns_children_{true}
  {}
  Node(leaf_t /*unused*/, uint32_t shortkey)
    : content_(std::in_place_type<std::atomic<ValueBucket<Value> *>>),
      key_{shortkey}, key_len_{32}
  {}

  Node(branch_t /*unused*/, uint32_t shortkey, uint8_t key_len, Node *child)
    : content_(std::in_place_type<std::array<std::atomic<Node<Value> *>, 16>>),
      key_{shortkey}, key_len_{key_len}
  {
    assert(child != nullptr);
    assert(get_shortkey_match_len(key_, key_len_, child->key_) == key_len);
    assert(key_len_ < child->key_len_);

    uint8_t branch_bits = get_branch_bits(child->key_, key_len_);
    std::get<std::array<std::atomic<Node<Value> *>, 16>>(content_)[branch_bits]
        .store(child, std::memory_order_release);
  }

  void set_owns_children() { owns_children_ = true; }

  ~Node()
  {
    if (!owns_children_) {
      return;
    }

    if (std::holds_alternative<std::array<std::atomic<Node<Value> *>, 16>>(
            content_)) {
      for (auto &ptr :
           std::get<std::array<std::atomic<Node<Value> *>, 16>>(content_)) {
        delete ptr.load();
      }
    }
  }

  uint8_t get_shortkey_match_len(uint32_t shortkey) const
  {
    return get_shortkey_match_len(key_, key_len_, shortkey);
  }

  // key2 is presumed to be a full length shortkey, i.e. 32 bits
  constexpr static uint8_t
  get_shortkey_match_len(uint32_t key1, uint8_t key1_len, uint32_t key2)
  {
    return std::min<uint8_t>(std::countl_zero(key1 ^ key2), key1_len) & 0xFC;
  }

  constexpr static uint8_t get_branch_bits(uint32_t key, uint8_t branch_point)
  {
    assert(branch_point < 32);
    assert(branch_point % 4 == 0);
    return (key >> (28 - branch_point)) & 0xF;
  }

  template<typename... Args> requires std::constructible_from<Value, Args...>
  Value *insert_value(std::span<const uint8_t> key, Args &&...args)
  {
    assert(std::holds_alternative<std::atomic<ValueBucket<Value> *>>(content_));
    auto &value_bucket = std::get<std::atomic<ValueBucket<Value> *>>(content_);

    return ValueBucket<Value>::insert(&value_bucket, key,
                                      std::forward<Args>(args)...);
  }

  Node<Value> *get_child(uint8_t branch_bits) const
  {
    assert((std::holds_alternative<std::array<std::atomic<Node<Value> *>, 16>>(
        content_)));
    return std::get<std::array<std::atomic<Node<Value> *>, 16>>(
               content_)[branch_bits]
        .load(std::memory_order_acquire);
  }

  bool try_insert_child(uint8_t branch_bits, Node<Value> *&expect,
                        Node<Value> *desired)
  {
    assert((std::holds_alternative<std::array<std::atomic<Node<Value> *>, 16>>(
        content_)));
    return std::get<std::array<std::atomic<Node<Value> *>, 16>>(
               content_)[branch_bits]
        .compare_exchange_strong(expect, desired, std::memory_order_release,
                                 std::memory_order_relaxed);
  }

  uint32_t get_key() const { return key_; }
  uint8_t get_key_len() const { return key_len_; }

private:
  std::variant<std::atomic<ValueBucket<Value> *>,
               std::array<std::atomic<Node<Value> *>, 16>>
      content_;

  const uint32_t key_;
  const uint8_t key_len_;
  bool owns_children_ = false;
};

} // namespace detail

template<typename Value, typename Hasher = std::hash<std::span<const uint8_t>>> class ConcurrentMap {
public:
  ConcurrentMap(Hasher hasher = Hasher()) : root_(typename detail::Node<Value>::root_t{}), hasher_(hasher) {}

  template<typename... Args> requires std::constructible_from<Value, Args...>
  Value *insert_or_get(std::span<const uint8_t> key,
                       Args &&...args)
  {
    uint32_t shortkey = hasher_(key);
    using node_t = typename detail::Node<Value>;
    node_t *current_node = &root_;

    while (true) {

      uint8_t match_len = current_node->get_shortkey_match_len(shortkey);
      assert(match_len == current_node->get_key_len());

      // We've found the node for the shortkey.
      if (current_node->get_key_len() == 32) {
        assert(current_node->get_key() == shortkey);
        return current_node->insert_value(key, std::forward<Args>(args)...);
      }

      // full match on current node, try recursing down to child

      uint8_t branch_bits =
          node_t::get_branch_bits(shortkey, current_node->get_key_len());
      node_t *child = current_node->get_child(branch_bits);

      if (child == nullptr) {
        // try and insert a new leaf node
        auto *new_leaf = new node_t(typename node_t::leaf_t{}, shortkey);

        if (current_node->try_insert_child(branch_bits, child, new_leaf)) {
          return new_leaf->insert_value(key, std::forward<Args>(args)...);
        }
        delete new_leaf;
        cpu_relax();
        continue;
      }

      uint8_t child_match_len = child->get_shortkey_match_len(shortkey);

      if (child_match_len == child->get_key_len()) {
        // full match on child, recursing down
        current_node = child;
        continue;
      }

      // partial match on child, try inserting new branch node
      auto *new_branch = new node_t(typename node_t::branch_t{}, shortkey,
                                    child_match_len, child);
      if (current_node->try_insert_child(branch_bits, child, new_branch)) {
        current_node = new_branch;
        continue;
      }
      delete new_branch;
      cpu_relax();
    }
  }

private:
  detail::Node<Value> root_;
  Hasher hasher_;
};

} // namespace hogd
