#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/merkle/allocator.h"

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

  const KeyType key_;
  Value value_;
  // Does not own the next_ptr_.  Memory is freed when the bump allocator is
  // destroyed.
  std::atomic<value_bucket_t *> next_ptr_;

  template<typename Func>
  requires std::invocable<Func, std::pair<const KeyType &, Value &>>
  static void for_each(std::atomic<value_bucket_t *> *scan_ptr, Func &f)
  {
    while (auto *next_value_bucket =
               scan_ptr->load(std::memory_order_relaxed)) {
      f(std::make_pair(std::cref(next_value_bucket->key_),
                       std::ref(next_value_bucket->value_)));
      scan_ptr = &next_value_bucket->next_ptr_;
    }
  }

  template<typename Allocator, typename... Args>
  requires std::constructible_from<Value, Args...>
  static Value *insert(std::atomic<value_bucket_t *> *scan_ptr,
                       Allocator &allocator, const KeyType &key, Args &&...args)
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
          new_value_bucket = allocator.alloc();
          CHECK_PTR(new_value_bucket);
          new (new_value_bucket)
              value_bucket_t{key, Value(std::forward<Args>(args)...)};
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

} // namespace detail

template<typename KeyType, typename Value, typename Hasher = std::hash<KeyType>>
class ConcurrentMap {
public:
  using shortkey_t = uint16_t;
  using value_bucket_t = detail::ValueBucket<KeyType, Value>;

  ConcurrentMap(size_t n_buckets = 1'048'576, Hasher hasher = Hasher())
    : buckets_(n_buckets), hasher_(hasher)
  {
    assert(std::has_single_bit(n_buckets));
  }

  static_assert(
      std::unsigned_integral<decltype(Hasher()(std::declval<KeyType>()))>);
  static_assert(sizeof(decltype(Hasher()(std::declval<KeyType>()))) >=
                    sizeof(uint64_t),
                "insufficient entropy otherwise");

  template<typename... Args> requires std::constructible_from<Value, Args...>
  Value *insert_or_get(const KeyType &key, Args &&...args)
  {
    uint64_t overall_shortkey = hasher_(key);
    size_t bucket_index = overall_shortkey & (buckets_.size() - 1);

    assert(buckets_.size() > bucket_index);

    auto *bucket = &buckets_[bucket_index];
    return value_bucket_t::insert(bucket, value_allocator_, key,
                                  std::forward<Args>(args)...);
  }

  template<typename Func>
  requires std::invocable<Func, std::pair<const KeyType &, Value &>>
  void for_each_parallel(Func f, size_t width = 1024,
                         WorkerPool &pool = WorkerPool::get_default_pool())
  {
    pool.for_each_parallel(
        buckets_,
        [&f](std::atomic<value_bucket_t *> &bucket) {
          value_bucket_t::for_each(&bucket, f);
        },
        width);
  }

private:
  std::vector<std::atomic<value_bucket_t *>> buckets_;

  BumpAllocator<value_bucket_t, 8192> value_allocator_;
  Hasher hasher_;
};

} // namespace hogd
