#pragma once

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include <array>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <variant>
#include <vector>

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
  static Value *insert(ValueBucket<Value> *value_bucket,
                       std::span<const uint8_t> key, Args &&...args)
  {
    ValueBucket<Value> *new_value_bucket = nullptr;
    ValueBucket<Value> *next_value_bucket =
        value_bucket->next_ptr_.load(std::memory_order_relaxed);

    while (true) {
      if (key_matches(std::span(value_bucket->key_), key)) {
        delete new_value_bucket;
        return &value_bucket->value_;
      }

      // try to insert at the end of the list
      if (next_value_bucket == nullptr) {
        // make sure that new_value_bucket points to a valid new object
        if (!new_value_bucket) {
          new_value_bucket =
              new ValueBucket<Value>{key, Value(std::forward<Args>(args)...)};
        }
        else {
          assert(key_matches(std::span(new_value_bucket->key_), key));
        }

        if (value_bucket->next_ptr_.compare_exchange_strong(
                next_value_bucket, new_value_bucket, std::memory_order_release,
                std::memory_order_relaxed)) {
          return &new_value_bucket->value_;
        }

        cpu_relax();
        continue;
      }

      // there are elements left in the list; continue to find them
      value_bucket = next_value_bucket;
      next_value_bucket =
          next_value_bucket->next_ptr_.load(std::memory_order_relaxed);
    }
  }
};

template<typename Value> class Node {

  ~Node()
  {
    if (std::holds_alternative<std::array<std::atomic<Node<Value> *>, 16>>(
            content_)) {
      for (auto &ptr :
           std::get<std::array<std::atomic<Node<Value> *>, 16>>(content_)) {
        delete ptr.load();
      }
    }
  }

private:
  template<typename... Args> requires std::constructible_from<Value, Args...>
  Value *insert_value(std::span<const std::byte> key, Args &&...args)
  {
    assert(std::holds_alternative<ValueBucket<Value>>(content_));
    auto *value_bucket = &std::get<ValueBucket<Value>>(content_);

    ValueBucket<Value> *new_value_bucket = nullptr;
    ValueBucket<Value> *next_value_bucket =
        value_bucket->next_ptr_.load(std::memory_order_relaxed);

    while (true) {
      if (value_bucket->key_ == key) {
        delete new_value_bucket;
        return &value_bucket->value_;
      }

      // try to insert at the end of the list
      if (next_value_bucket == nullptr) {
        // make sure that new_value_bucket points to a valid new object
        if (!new_value_bucket) {
          new_value_bucket =
              new ValueBucket<Value>{key, Value(std::forward<Args>(args)...)};
        }
        else {
          assert(new_value_bucket->key_ == key);
        }

        if (value_bucket->next_ptr_.compare_exchange_strong(
                next_value_bucket, new_value_bucket, std::memory_order_release,
                std::memory_order_relaxed)) {
          return &new_value_bucket->value_;
        }

        cpu_relax();
        continue;
      }

      // there are elements left in the list; continue to find them
      value_bucket = next_value_bucket;
      next_value_bucket =
          next_value_bucket->next_ptr_.load(std::memory_order_relaxed);
    }
  }

  const uint32_t key_;
  const uint8_t key_len_;
  std::variant<ValueBucket<Value>, std::array<std::atomic<Node<Value> *>, 16>>
      content_;
};

} // namespace detail

class ConcurrentMap {};

} // namespace hogd
