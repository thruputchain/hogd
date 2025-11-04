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
#include <utility>
#include <variant>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"

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

  template<typename Func> requires std::invocable<Func, Value &>
  static void for_each(std::atomic<ValueBucket<Value> *> *scan_ptr, Func &f)
  {
    while (auto *next_value_bucket =
               scan_ptr->load(std::memory_order_relaxed)) {
      f(next_value_bucket->value_);
      scan_ptr = &next_value_bucket->next_ptr_;
    }
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
          // This call to construct new_value_bucket only happens at most once.
          // NOLINTBEGIN(bugprone-use-after-move)
          new_value_bucket =
              new ValueBucket<Value>{key, Value(std::forward<Args>(args)...)};
          // NOLINTEND(bugprone-use-after-move)
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

template<typename Value, std::unsigned_integral Shortkey = uint16_t>
class Node {
public:
  constexpr static size_t kShortkeyBits = sizeof(Shortkey) * 8;

  // NOLINTBEGIN(readability-identifier-naming)
  struct leaf_t {};
  struct branch_t {};
  // NOLINTEND(readability-identifier-naming)

  Node()
    : content_(std::in_place_type<
               std::array<std::atomic<Node<Value, Shortkey> *>, 16>>),
      key_{0}, key_len_{0}, owns_children_{true}
  {}
  Node(leaf_t /*unused*/, Shortkey shortkey)
    : content_(std::in_place_type<std::atomic<ValueBucket<Value> *>>),
      key_{shortkey}, key_len_{kShortkeyBits}, owns_children_{true}
  {}

  Node(branch_t /*unused*/, Shortkey shortkey, uint8_t key_len, Node *child)
    : content_(std::in_place_type<
               std::array<std::atomic<Node<Value, Shortkey> *>, 16>>),
      key_{shortkey}, key_len_{key_len}
  {
    assert(child != nullptr);
    assert(get_shortkey_match_len(key_, key_len_, child->key_) == key_len);
    assert(key_len_ < child->key_len_);

    uint8_t branch_bits = get_branch_bits(child->key_, key_len_);
    std::get<std::array<std::atomic<Node<Value, Shortkey> *>, 16>>(
        content_)[branch_bits]
        .store(child, std::memory_order_release);
  }

  void set_owns_children() { owns_children_ = true; }

  ~Node()
  {
    if (!owns_children_) {
      return;
    }

    // get_if is noexcept
    if (std::holds_alternative<std::array<std::atomic<Node<Value> *>, 16>>(
            content_)) {
      auto &content =
          *std::get_if<std::array<std::atomic<Node<Value> *>, 16>>(&content_);
      for (auto &ptr : content) {
        delete ptr.load();
      }
    }

    if (std::holds_alternative<std::atomic<ValueBucket<Value> *>>(content_)) {
      auto &content =
          *std::get_if<std::atomic<ValueBucket<Value> *>>(&content_);
      delete content.load();
    }
  }

  template<typename Func> requires std::invocable<Func, Value &>
  void for_each(Func &f)
  {
    if (std::holds_alternative<std::atomic<ValueBucket<Value> *>>(content_)) {
      auto &content = std::get<std::atomic<ValueBucket<Value> *>>(content_);
      ValueBucket<Value>::for_each(&content, f);
      return;
    }

    assert((std::holds_alternative<
            std::array<std::atomic<Node<Value, Shortkey> *>, 16>>(content_)));

    auto &content =
        std::get<std::array<std::atomic<Node<Value, Shortkey> *>, 16>>(
            content_);
    for (auto &ptr : content) {
      auto *node = ptr.load();
      if (node == nullptr)
        continue;
      node->for_each(f);
    }
  }

  uint8_t get_shortkey_match_len(Shortkey shortkey) const
  {
    return get_shortkey_match_len(key_, key_len_, shortkey);
  }

  // key2 is presumed to be a full length shortkey, i.e. 32 bits
  constexpr static uint8_t
  get_shortkey_match_len(Shortkey key1, uint8_t key1_len, Shortkey key2)
  {
    Shortkey diff = key1 ^ key2;
    return std::min<uint8_t>(std::countl_zero(diff), key1_len) & 0xFC;
  }

  constexpr static uint8_t get_branch_bits(Shortkey key, uint8_t branch_point)
  {
    constexpr size_t kShortkeyBits = sizeof(Shortkey) * 8;
    assert(branch_point < kShortkeyBits);
    assert(branch_point % 4 == 0);
    return (key >> (kShortkeyBits - 4 - branch_point)) & 0xF;
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

  Shortkey get_key() const { return key_; }
  uint8_t get_key_len() const { return key_len_; }

private:
  std::variant<std::atomic<ValueBucket<Value> *>,
               std::array<std::atomic<Node<Value, Shortkey> *>, 16>>
      content_;

  const Shortkey key_;
  const uint8_t key_len_;
  bool owns_children_ = false;
};

} // namespace detail

template<typename Value, typename Hasher = std::hash<std::span<const uint8_t>>>
class ConcurrentMap {
public:
  using shortkey_t = uint16_t;

  ConcurrentMap(size_t n_roots = available_parallelism(),
                Hasher hasher = Hasher())
    : roots_(n_roots), hasher_(hasher)
  {}

  static_assert(std::unsigned_integral<
                decltype(Hasher()(std::declval<std::span<const uint8_t>>()))>);
  static_assert(
      sizeof(decltype(Hasher()(std::declval<std::span<const uint8_t>>()))) >=
          sizeof(uint64_t),
      "insufficient entropy otherwise");

  template<typename... Args> requires std::constructible_from<Value, Args...>
  Value *insert_or_get(std::span<const uint8_t> key, Args &&...args)
  {
    uint64_t overall_shortkey = hasher_(key);
    uint32_t root_selector = (overall_shortkey >> 32) % roots_.size();
    uint16_t shortkey = overall_shortkey & 0xFFFF;

    assert(roots_.size() > root_selector);

    using node_t = typename detail::Node<Value>;
    node_t *current_node = &roots_[root_selector];

    while (true) {

      uint8_t match_len = current_node->get_shortkey_match_len(shortkey);
      assert(match_len == current_node->get_key_len());

      // We've found the node for the shortkey.
      if (current_node->get_key_len() == kShortkeyBits) {
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

  template<typename Func> requires std::invocable<Func, Value &>
  void for_each_parallel(Func f, size_t width = kDefaultParallelRangeWidth,
                         WorkerPool &pool = WorkerPool::get_default_pool())
  {
    pool.for_each_parallel(
        roots_,
        [&f](detail::Node<Value, shortkey_t> &root) { root.for_each(f); },
        width);
  }

private:
  constexpr static size_t kShortkeyBits = sizeof(shortkey_t) * 8;

  std::vector<detail::Node<Value, shortkey_t>> roots_;
  Hasher hasher_;
};

} // namespace hogd
