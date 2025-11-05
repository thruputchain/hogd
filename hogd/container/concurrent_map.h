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
#include <utility>
#include <variant>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
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
template<typename KeyType, typename Value,
         typename KeyEqual = std::equal_to<KeyType>>
class ValueBucket {
public:
  template<typename... Args> requires std::constructible_from<Value, Args...>
  ValueBucket(KeyType key, Args &&...args)
    : key_(std::move(key)), value_(std::forward<Args>(args)...)
  {}

  using value_bucket_t = ValueBucket<KeyType, Value, KeyEqual>;

  ValueBucket(const ValueBucket &) = delete;
  ValueBucket &operator=(const ValueBucket &) = delete;
  ValueBucket(ValueBucket &&) noexcept = delete;
  ValueBucket &operator=(ValueBucket &&) noexcept = delete;

  ~ValueBucket()
  {
    auto *next_ptr = &next_ptr_;
    for(;;) {
      assert(next_ptr != nullptr);
      auto* next = next_ptr->load(std::memory_order_relaxed);
      if (next) {
        next_ptr = &next->next_ptr_;
        delete next;
      } else {
        break;
      }
    }
  }

  const KeyType key_;
  Value value_;
  std::atomic<value_bucket_t *> next_ptr_;

  template<typename Func> requires std::invocable<Func, Value &>
  static void for_each(std::atomic<value_bucket_t *> *scan_ptr, Func &f)
  {
    while (auto *next_value_bucket =
               scan_ptr->load(std::memory_order_relaxed)) {
      f(next_value_bucket->value_);
      scan_ptr = &next_value_bucket->next_ptr_;
    }
  }

  template<typename... Args> requires std::constructible_from<Value, Args...>
  static Value *insert(std::atomic<value_bucket_t *> *scan_ptr,
                       const KeyType &key, Args &&...args)
  {
    // The value bucket that we might create in this iteration of insert.
    // Starts as nullptr, instantiated if we try to insert.
    value_bucket_t *new_value_bucket = nullptr;

    value_bucket_t *next_value_bucket =
        scan_ptr->load(std::memory_order_relaxed);

    while (true) {
      // If we're at the end of the list, try to insert.
      if (next_value_bucket == nullptr) {

        if (!new_value_bucket) {
          // This call to construct new_value_bucket only happens at most once.
          // NOLINTBEGIN(bugprone-use-after-move)
          new_value_bucket =
              new value_bucket_t{key, Value(std::forward<Args>(args)...)};
          // NOLINTEND(bugprone-use-after-move)
        }
        assert(KeyEqual{}(new_value_bucket->key_, key));

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
      if (KeyEqual{}(next_value_bucket->key_, key)) {
        delete new_value_bucket;
        return &next_value_bucket->value_;
      }

      // Continue scanning the list
      scan_ptr = &next_value_bucket->next_ptr_;
      next_value_bucket = scan_ptr->load(std::memory_order_relaxed);
    }
  }
};

template<typename KeyType, typename Value,
         std::unsigned_integral Shortkey = uint16_t,
         typename KeyEqual = std::equal_to<KeyType>>
class Node {
public:
  constexpr static size_t kShortkeyBits = sizeof(Shortkey) * 8;

  using node_t = Node<KeyType, Value, Shortkey, KeyEqual>;
  using value_bucket_t = ValueBucket<KeyType, Value, KeyEqual>;

  // NOLINTBEGIN(readability-identifier-naming)
  struct leaf_t {};
  struct branch_t {};
  // NOLINTEND(readability-identifier-naming)

  Node()
    : content_(std::in_place_type<std::array<std::atomic<node_t *>, 16>>),
      key_{0}, key_len_{0}, owns_children_{true}
  {}
  Node(leaf_t /*unused*/, Shortkey shortkey)
    : content_(std::in_place_type<std::atomic<value_bucket_t *>>),
      key_{shortkey}, key_len_{kShortkeyBits}, owns_children_{true}
  {}

  Node(branch_t /*unused*/, Shortkey shortkey, uint8_t key_len, Node *child)
    : content_(std::in_place_type<std::array<std::atomic<node_t *>, 16>>),
      key_{shortkey}, key_len_{key_len}
  {
    assert(child != nullptr);
    assert(get_shortkey_match_len(key_, key_len_, child->key_) == key_len);
    assert(key_len_ < child->key_len_);

    uint8_t branch_bits = get_branch_bits(child->key_, key_len_);
    std::get<std::array<std::atomic<node_t *>, 16>>(content_)[branch_bits]
        .store(child, std::memory_order_release);
  }

  void set_owns_children() { owns_children_ = true; }

  ~Node()
  {
    if (!owns_children_) {
      return;
    }

    // get_if is noexcept
    if (std::holds_alternative<std::array<std::atomic<node_t *>, 16>>(
            content_)) {
      auto &content =
          *std::get_if<std::array<std::atomic<node_t *>, 16>>(&content_);
      for (auto &ptr : content) {
        delete ptr.load();
      }
    }

    if (std::holds_alternative<std::atomic<value_bucket_t *>>(content_)) {
      auto &content = *std::get_if<std::atomic<value_bucket_t *>>(&content_);
      delete content.load();
    }
  }

  // this is a limited, bounded depth recursion; the I parameter bypasses
  // clang-tidy's recursion limit
  template<size_t I, typename Func>
  requires std::invocable<Func, Value &> && (I <= sizeof(Shortkey) * 2)
  void for_each(Func &f)
  {
    if (std::holds_alternative<std::atomic<value_bucket_t *>>(content_)) {
      auto &content = std::get<std::atomic<value_bucket_t *>>(content_);
      value_bucket_t::for_each(&content, f);
      return;
    }

    assert((std::holds_alternative<std::array<std::atomic<node_t *>, 16>>(
        content_)));

    auto &content = std::get<std::array<std::atomic<node_t *>, 16>>(content_);
    for (auto &ptr : content) {
      auto *node = ptr.load();
      if (node == nullptr)
        continue;
      if constexpr (I < sizeof(Shortkey) * 2) {
        node->template for_each<I + 1>(f);
      }
      else {
        panic("Unreachable");
      }
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
  Value *insert_value(const KeyType &key, Args &&...args)
  {
    assert(std::holds_alternative<std::atomic<value_bucket_t *>>(content_));
    auto &value_bucket = std::get<std::atomic<value_bucket_t *>>(content_);

    return value_bucket_t::insert(&value_bucket, key,
                                  std::forward<Args>(args)...);
  }

  node_t *get_child(uint8_t branch_bits) const
  {
    assert((std::holds_alternative<std::array<std::atomic<node_t *>, 16>>(
        content_)));
    return std::get<std::array<std::atomic<node_t *>, 16>>(
               content_)[branch_bits]
        .load(std::memory_order_acquire);
  }

  bool try_insert_child(uint8_t branch_bits, node_t *&expect, node_t *desired)
  {
    assert((std::holds_alternative<std::array<std::atomic<node_t *>, 16>>(
        content_)));
    assert(desired != nullptr);
    assert(expect == nullptr || expect->key_len_ > desired->key_len_);
    return std::get<std::array<std::atomic<node_t *>, 16>>(
               content_)[branch_bits]
        .compare_exchange_strong(expect, desired, std::memory_order_release,
                                 std::memory_order_relaxed);
  }

  Shortkey get_key() const { return key_; }
  uint8_t get_key_len() const { return key_len_; }

private:
  std::variant<std::atomic<value_bucket_t *>,
               std::array<std::atomic<node_t *>, 16>>
      content_;

  const Shortkey key_;
  const uint8_t key_len_;
  bool owns_children_ = false;
};

} // namespace detail

template<typename KeyType, typename Value, typename Hasher = std::hash<KeyType>>
class ConcurrentMap {
public:
  using shortkey_t = uint16_t;

  ConcurrentMap(size_t n_roots = available_parallelism(),
                Hasher hasher = Hasher())
    : roots_(n_roots), hasher_(hasher)
  {}

  static_assert(
      std::unsigned_integral<decltype(Hasher()(std::declval<KeyType>()))>);
  static_assert(sizeof(decltype(Hasher()(std::declval<KeyType>()))) >=
                    sizeof(uint64_t),
                "insufficient entropy otherwise");

  template<typename... Args> requires std::constructible_from<Value, Args...>
  Value *insert_or_get(const KeyType &key, Args &&...args)
  {
    uint64_t overall_shortkey = hasher_(key);
    uint32_t root_selector = (overall_shortkey >> 32) % roots_.size();
    uint16_t shortkey = overall_shortkey & 0xFFFF;

    assert(roots_.size() > root_selector);

    using node_t = typename detail::Node<KeyType, Value, shortkey_t>;
    node_t *current_node = &roots_[root_selector];

    while (true) {

      assert(current_node->get_shortkey_match_len(shortkey) ==
             current_node->get_key_len());

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
        new_branch->set_owns_children();
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
        [&f](detail::Node<KeyType, Value, shortkey_t> &root) {
          root.template for_each<0>(f);
        },
        width);
  }

private:
  constexpr static size_t kShortkeyBits = sizeof(shortkey_t) * 8;

  std::vector<detail::Node<KeyType, Value, shortkey_t>> roots_;
  Hasher hasher_;
};

} // namespace hogd
