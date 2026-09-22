#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <x23/nodestroy.h>

#include "hogd/base/cleanup.h"
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
// only performs atomic read operations (i.e. no shared locks).

namespace detail {

// Linked list of values, to resolve hash collisions.
// These are kept in an unsorted (arbitrary) order.  Items are appended to the
// end.
template<typename KeyType, typename Value,
         typename KeyEqual = std::equal_to<KeyType>>
class alignas(kCacheLineSize) ValueBucket {
public:
  template<typename... Args> requires std::constructible_from<Value, Args...>
  ValueBucket(KeyType key, Args &&...args)
    : key_(std::move(key)), next_ptr_(nullptr),
      value_(std::forward<Args>(args)...)
  {}

  using value_bucket_t = ValueBucket<KeyType, Value, KeyEqual>;

  ValueBucket(const ValueBucket &) = delete;
  ValueBucket &operator=(const ValueBucket &) = delete;
  ValueBucket(ValueBucket &&) noexcept = delete;
  ValueBucket &operator=(ValueBucket &&) noexcept = delete;

  const KeyType key_;
  // Does not own the next_ptr_.  Memory is freed when the bump allocator is
  // destroyed.
  std::atomic<value_bucket_t *> next_ptr_;
  // Hardcoded to 64 (x86 cache line) rather than kCacheLineSize, which can
  // be 256 on aarch64 and would waste too much padding.
  alignas(64) Value value_;

  // You almost certainly do not want to do an insert concurrent with a
  // for_each. In such a situation, the insert() might or might not get picked
  // up the the for_each.
  template<typename Func>
  requires std::invocable<Func, std::pair<const KeyType &, Value &>>
  static void for_each(std::atomic<value_bucket_t *> *scan_ptr, Func &f)
  {
    while (auto *next_value_bucket =
               scan_ptr->load(std::memory_order_acquire)) {
      f(std::make_pair(std::cref(next_value_bucket->key_),
                       std::ref(next_value_bucket->value_)));
      scan_ptr = &next_value_bucket->next_ptr_;
    }
  }

  // DO NOT insert concurrent with a for_each_destructive.
  template<typename Func>
  requires std::invocable<Func, std::pair<const KeyType &, Value &>>
  static void for_each_destructive(value_bucket_t *value_bucket, Func &f)
  {
    while (value_bucket != nullptr) {
      auto *next_bucket =
          value_bucket->next_ptr_.load(std::memory_order_acquire);
      try {
        f(std::make_pair(std::cref(value_bucket->key_),
                         std::ref(value_bucket->value_)));
      } catch (...) {
        value_bucket->~value_bucket_t();
        clear(next_bucket);
        throw;
      }
      value_bucket->~value_bucket_t();
      value_bucket = next_bucket;
    }
  }

  static Value *get(std::atomic<value_bucket_t *> *scan_ptr, const KeyType &key)
  {
    // acquire to synchronize with the last insertion.
    while (auto *next_value_bucket =
               scan_ptr->load(std::memory_order_acquire)) {
      if (KeyEqual{}(next_value_bucket->key_, key)) {
        return &next_value_bucket->value_;
      }
      scan_ptr = &next_value_bucket->next_ptr_;
    }
    return nullptr;
  }

  static void clear(value_bucket_t *value_bucket)
  {
    while (value_bucket != nullptr) {
      value_bucket_t *next_value_bucket =
          value_bucket->next_ptr_.load(std::memory_order_acquire);
      value_bucket->~value_bucket_t();
      value_bucket = next_value_bucket;
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
        scan_ptr->load(std::memory_order_acquire);

    while (true) {
      // If we're at the end of the list, try to insert.
      if (next_value_bucket == nullptr) {

        if (!new_value_bucket) {
          // This call to construct new_value_bucket only happens at most once.
          // NOLINTBEGIN(bugprone-use-after-move)
          new_value_bucket = allocator.alloc();
          CHECK_PTR(new_value_bucket);
          new (new_value_bucket)
              value_bucket_t{key, std::forward<Args>(args)...};
          // NOLINTEND(bugprone-use-after-move)
        }
        assert(KeyEqual{}(new_value_bucket->key_, key));

        if (scan_ptr->compare_exchange_strong(next_value_bucket,
                                              new_value_bucket,
                                              std::memory_order_release,
                                              std::memory_order_acquire)) {
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
        if (new_value_bucket) {
          new_value_bucket->~value_bucket_t();
        }
        return &next_value_bucket->value_;
      }

      // Continue scanning the list
      scan_ptr = &next_value_bucket->next_ptr_;
      next_value_bucket = scan_ptr->load(std::memory_order_acquire);
    }
  }
};

} // namespace detail

// BucketAllocator allocates the bucket-head array; it exists so tests can
// account for those allocations.
template<typename KeyType, typename Value, typename Hasher = std::hash<KeyType>,
         typename BucketAllocator =
             std::allocator<std::atomic<detail::ValueBucket<KeyType, Value> *>>>
class ConcurrentMap {
public:
  using shortkey_t = uint16_t;
  using value_bucket_t = detail::ValueBucket<KeyType, Value>;
  using bucket_array_t =
      std::vector<std::atomic<value_bucket_t *>, BucketAllocator>;

  // Upper bound on the cleared bucket arrays retained for reuse by maps of
  // this type; see BucketArrayPool below.
  static constexpr size_t kMaxPooledBucketArrays = 4;

  ConcurrentMap(size_t n_buckets = 1'048'576, Hasher hasher = Hasher())
    : buckets_(acquire_bucket_array(n_buckets)), hasher_(hasher)
  {
    assert(std::has_single_bit(n_buckets));
  }

  ~ConcurrentMap()
  {
    if (!cleared_) {
      clear_slow();
    }
    // All clearing paths leave every head nullptr, so the array can be
    // reused as-is.
    release_bucket_array(std::move(buckets_));
  }

  static_assert(
      std::unsigned_integral<decltype(Hasher()(std::declval<KeyType>()))>);
  static_assert(sizeof(decltype(Hasher()(std::declval<KeyType>()))) >=
                    sizeof(uint64_t),
                "insufficient entropy otherwise");

  Value *get(const KeyType &key)
  {
    assert(!cleared_);
    uint64_t overall_shortkey = hasher_(key);
    size_t bucket_index = overall_shortkey & (buckets_.size() - 1);

    assert(buckets_.size() > bucket_index);

    auto *bucket = &buckets_[bucket_index];
    return value_bucket_t::get(bucket, key);
  }

  template<typename... Args> requires std::constructible_from<Value, Args...>
  Value *insert_or_get(const KeyType &key, Args &&...args)
  {
    assert(!cleared_);
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
    assert(!cleared_);
    pool.for_each_parallel(
        buckets_,
        [&f](std::atomic<value_bucket_t *> &bucket) {
          value_bucket_t::for_each(&bucket, f);
        },
        width);
  }

  // This variant clears the associated value buckets in the map as
  // it performs the iteration.
  template<typename Func>
  requires std::invocable<Func, std::pair<const KeyType &, Value &>>
  void for_each_parallel_destructive(
      Func f, size_t width = 1024,
      WorkerPool &pool = WorkerPool::get_default_pool())
  {
    assert(!cleared_);
    std::atomic<size_t> cleared_count = 0;
    std::vector<std::future<void>> futures;
    futures.reserve(pool.worker_count());
    Defer drain{[&futures] {
      for (auto &f : futures)
        f.wait();
    }};
    for (size_t i = 0; i < pool.worker_count(); i++) {
      futures.push_back(pool.submit([&] {
        while (true) {
          size_t start =
              cleared_count.fetch_add(width, std::memory_order_relaxed);
          size_t end = std::min<size_t>(start + width, buckets_.size());
          for (size_t i = start; i < end; i++) {
            // No inserts run concurrently with a destructive iteration, so
            // load + store avoids a locked RMW on every (mostly empty) bucket.
            value_bucket_t *bucket =
                buckets_[i].load(std::memory_order_acquire);
            if (bucket == nullptr) {
              continue;
            }
            buckets_[i].store(nullptr, std::memory_order_relaxed);
            value_bucket_t::for_each_destructive(bucket, f);
          }
          if (start + width >= buckets_.size()) {
            return;
          }
        }
      }));
    }
    drain.reset();
    for (auto &future : futures) {
      // Rethrows the exception, if it exists.
      future.get();
    }
    cleared_ = true;
  }

  // Like for_each_parallel but computes width to produce roughly
  // worker_count() jobs, avoiding excessive dispatch overhead when
  // scanning the full bucket array.
  template<typename Func>
  requires std::invocable<Func, std::pair<const KeyType &, Value &>>
  void for_each_parallel_among_workers(
      Func f, WorkerPool &pool = WorkerPool::get_default_pool())
  {
    for_each_parallel(std::move(f), worker_width(pool), pool);
  }

  template<typename Func>
  requires std::invocable<Func, std::pair<const KeyType &, Value &>>
  void for_each_parallel_destructive_among_workers(
      Func f, WorkerPool &pool = WorkerPool::get_default_pool())
  {
    for_each_parallel_destructive(std::move(f), worker_width(pool), pool);
  }

  void clear(WorkerPool &pool = WorkerPool::get_default_pool(),
             size_t width = 1024)
  {
    assert(!cleared_);
    pool.for_each_parallel(
        buckets_,
        [](std::atomic<value_bucket_t *> &bucket) {
          // See for_each_parallel_destructive() for why this is not an
          // exchange().
          value_bucket_t *head = bucket.load(std::memory_order_acquire);
          if (head == nullptr) {
            return;
          }
          bucket.store(nullptr, std::memory_order_relaxed);
          value_bucket_t::clear(head);
        },
        width);
    cleared_ = true;
  }

private:
  // Cleared bucket arrays are recycled through a small bounded per-map-type
  // pool, so a block-scoped map (e.g. BlockTX's lock maps) does not allocate
  // and zero a fresh head array (8MB by default) per block. It retains at
  // most kMaxPooledBucketArrays arrays, i.e. 32MB for default-size maps.
  struct BucketArrayPool {
    BucketArrayPool() { arrays.reserve(kMaxPooledBucketArrays); }

    std::mutex mutex;
    // Never grows past kMaxPooledBucketArrays, so with the reserve() above
    // release_bucket_array()'s push_back cannot allocate.
    std::vector<bucket_array_t> arrays;
  };

  static BucketArrayPool &bucket_array_pool()
  {
    // Never destroyed: a map owned by a static initialized before the pool
    // is destroyed after it, and must still be able to return its array.
    static x23::NoDestroy<BucketArrayPool> pool;
    return *pool;
  }

  static bucket_array_t acquire_bucket_array(size_t n_buckets)
  {
    {
      BucketArrayPool &pool = bucket_array_pool();
      std::scoped_lock g(pool.mutex);
      for (auto it = pool.arrays.begin(); it != pool.arrays.end(); ++it) {
        if (it->size() == n_buckets) {
          bucket_array_t buckets = std::move(*it);
          pool.arrays.erase(it);
          return buckets;
        }
      }
    }
    // Value-initializes every head to nullptr.
    return bucket_array_t(n_buckets);
  }

  // Requires all heads of |buckets| to be nullptr. Takes the array by value so
  // that it is freed here if the pool is full.
  static void release_bucket_array(bucket_array_t buckets) noexcept
  {
    if (buckets.empty()) {
      return;
    }
    BucketArrayPool &pool = bucket_array_pool();
    std::scoped_lock g(pool.mutex);
    if (pool.arrays.size() < kMaxPooledBucketArrays) {
      pool.arrays.push_back(std::move(buckets));
    }
  }

  size_t worker_width(WorkerPool &pool) const
  {
    return std::max<size_t>(1, buckets_.size() / pool.worker_count());
  }

  void clear_slow() noexcept
  {
    for (auto &bucket : buckets_) {
      value_bucket_t *head = bucket.load(std::memory_order_acquire);
      if (head == nullptr) {
        continue;
      }
      // Null the head too so the array can be recycled by the destructor.
      bucket.store(nullptr, std::memory_order_relaxed);
      value_bucket_t::clear(head);
    }
    cleared_ = true;
  }

  bucket_array_t buckets_;

  BumpAllocator<value_bucket_t, 8192> value_allocator_;
  Hasher hasher_;
  bool cleared_ = false;
};

} // namespace hogd
