// synchronized_map.h - provides a bucket hash table that exposes locking

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <concepts>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <ranges>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/thread_checked_mutex.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"

namespace hogd {

template<typename Lock>
concept mutex_like = requires(Lock lock) {
  { lock.lock() } -> std::same_as<void>;
  { lock.unlock() } -> std::same_as<void>;
  { lock.try_lock() } -> std::same_as<bool>;
};

static_assert(mutex_like<std::mutex>);

// Default bucket lock. Debug builds check that each bucket lock is unlocked on
// the thread that locked it, which catches a lock held across a co_await that
// resumes on another thread.
#ifdef NDEBUG
using synchronized_map_mutex = std::mutex;
#else
using synchronized_map_mutex = ThreadCheckedMutex<std::mutex>;
#endif
static_assert(mutex_like<synchronized_map_mutex>);

template<typename Lock>
concept shared_mutex_like = requires(Lock lock) {
  { lock.lock_shared() } -> std::same_as<void>;
  { lock.unlock_shared() } -> std::same_as<void>;
  { lock.try_lock_shared() } -> std::same_as<bool>;
} && mutex_like<Lock>;

static_assert(shared_mutex_like<std::shared_mutex>);

// SynchronizedMap is similar to a std::unordered_map, but provides scalable
// synchronization through a sharded hash table with buckets that are each
// protected by a separate lock.
//
// SynchronizedMap exposes these locks through a std::unique_lock in many of
// its operations, allowing the caller to control when the lock is released so
// it can synchronize access to the map's elements.
//
// Example:
// SynchronizedMap<int, std::string> map;
// auto [val, ul] = map[10];
// access to val is synchronized
// ul.unlock();
template<class Key, class T, class Hash = std::hash<Key>,
         class KeyEqual = std::equal_to<Key>,
         class Allocator = std::allocator<std::pair<const Key, T>>,
         mutex_like Lock = synchronized_map_mutex>
class SynchronizedMap {
public:
  // type definitions
  using key_type = Key;
  using map_type = T;
  using lock_type = Lock;
  using value_type = std::pair<const Key, T>;
  using size_type = size_t;
  using hasher = Hash;
  using key_equal = KeyEqual;
  using allocator_type = Allocator;

  SynchronizedMap()
    : locks_(scalable_shard_size()), buckets_(scalable_shard_size())
  {}
  SynchronizedMap(size_t count, const Hash &hash = Hash(),
                  const KeyEqual &equal = KeyEqual(),
                  const Allocator &alloc = Allocator())
    : hash_fn_(hash), locks_(count), buckets_(count, Map(1, hash, equal, alloc))
  {}
  template<std::input_iterator InputIt>
  SynchronizedMap(InputIt first, InputIt last,
                  size_t count = scalable_shard_size(),
                  const Hash &hash = Hash(),
                  const key_equal &equal = key_equal(),
                  const Allocator &alloc = Allocator())
    : hash_fn_(hash), locks_(count), buckets_(count, Map(1, hash, equal, alloc))
  {
    std::ranges::for_each(first, last, [this](const value_type &arg) {
      insert(arg.first, arg.second);
    });
  }
  SynchronizedMap(std::initializer_list<value_type> lst,
                  size_t count = scalable_shard_size(),
                  const Hash &hash = Hash(),
                  const key_equal &equal = key_equal(),
                  const Allocator &alloc = Allocator())
    : hash_fn_(hash), locks_(count), buckets_(count, Map(1, hash, equal, alloc))
  {
    std::ranges::for_each(lst, [this](const value_type &arg) {
      insert(arg.first, arg.second);
    });
  }
  ~SynchronizedMap() = default;

  // disable copy and support move.
  SynchronizedMap(const SynchronizedMap &map) = delete;
  SynchronizedMap &operator=(const SynchronizedMap &map) = delete;
  SynchronizedMap(SynchronizedMap &&map) noexcept = default;
  SynchronizedMap &operator=(SynchronizedMap &&map) noexcept = default;

  std::pair<T &, std::unique_lock<lock_type>> operator[](const Key &key)
  {
    auto [map, ul] = get_bucket(key);
    return {map[key], std::move(ul)};
  }

  // find with exclusive lock (for when you need to modify the value)
  std::pair<T *, std::unique_lock<lock_type>> find(const Key &key)
  {
    auto [map, ul] = get_bucket(key);
    auto it = map.find(key);
    if (it == map.end())
      return {nullptr, std::move(ul)};
    return {&it->second, std::move(ul)};
  }

  // find_shared with shared lock
  std::pair<const T *, std::shared_lock<lock_type>>
  find_shared(const Key &key) const requires shared_mutex_like<lock_type>
  {
    auto [map, sl] = get_bucket_shared(key);
    auto it = map.find(key);
    if (it == map.end())
      return {nullptr, std::move(sl)};
    return {&it->second, std::move(sl)};
  }

  T *find(const Key &key, const std::unique_lock<lock_type> &ul)
  {
    assert(ul.owns_lock());
    auto &map = get_bucket(key, ul);
    auto it = map.find(key);
    if (it == map.end())
      return nullptr;
    return &it->second;
  }

  template<typename... Args> requires std::constructible_from<T, Args...>
  std::pair<T *, std::unique_lock<lock_type>> insert(const Key &k,
                                                     Args &&...args)
  {
    auto [map, ul] = get_bucket(k);
    auto [it, inserted] =
        map.emplace(std::piecewise_construct, std::forward_as_tuple(k),
                    std::forward_as_tuple(std::forward<Args>(args)...));
    if (!inserted)
      return {nullptr, std::move(ul)};
    return {&it->second, std::move(ul)};
  }

  std::pair<T *, std::unique_lock<lock_type>> insert(const Key &k, const T &obj)
  {
    auto [map, ul] = get_bucket(k);
    auto [it, inserted] = map.insert({k, obj});
    if (!inserted)
      return {nullptr, std::move(ul)};
    return {&it->second, std::move(ul)};
  }

  T *insert(const Key &key, T &&obj, const std::unique_lock<lock_type> &ul)
  {
    assert(std::has_single_bit(locks_.size()));
    assert(locks_.size() == buckets_.size());
    assert(ul.owns_lock());

    size_t idx = hash_fn_(key) & (buckets_.size() - 1);
    if (ul.mutex() != std::addressof(locks_[idx])) [[unlikely]]
      panic("Wrong lock.");
    auto [it, inserted] = buckets_[idx].insert({key, std::move(obj)});
    if (!inserted)
      return nullptr;
    return &it->second;
  }

  std::tuple<T *, std::unique_lock<lock_type>, bool>
  find_or_insert(const Key &k, const T &obj)
  {
    auto [map, ul] = get_bucket(k);
    auto [it, inserted] = map.insert({k, obj});
    return {&it->second, std::move(ul), inserted};
  }

  std::pair<T *, std::unique_lock<lock_type>> insert_or_assign(const Key &k,
                                                               T &&obj)
  {
    auto [map, ul] = get_bucket(k);
    auto [it, inserted] = map.insert_or_assign(k, std::move(obj));
    return {&it->second, std::move(ul)};
  }

  size_t remove(const Key &key)
  {
    auto [map, ul] = get_bucket(key);
    return map.erase(key);
  }

  size_t remove(const Key &key, const std::unique_lock<lock_type> &ul)
  {
    assert(std::has_single_bit(locks_.size()));
    assert(locks_.size() == buckets_.size());
    assert(ul.owns_lock());

    size_t idx = hash_fn_(key) & (buckets_.size() - 1);
    if (ul.mutex() != std::addressof(locks_[idx])) [[unlikely]]
      panic("Wrong lock.");
    return buckets_[idx].erase(key);
  }

  size_t size()
  {
    assert(locks_.size() == buckets_.size());

    size_t ret = 0;
    for (size_t i = 0; i < locks_.size(); ++i) {
      std::scoped_lock guard(locks_[i]);
      ret += buckets_[i].size();
    }
    return ret;
  }

  void clear()
  {
    assert(locks_.size() == buckets_.size());

    for (size_t i = 0; i < locks_.size(); ++i) {
      std::scoped_lock guard(locks_[i]);
      buckets_[i].clear();
    }
  }

  // for_each executes func on each element.
  template<typename F> void for_each(F func)
  {
    assert(locks_.size() == buckets_.size());

    for (size_t i = 0; i < locks_.size(); ++i) {
      std::scoped_lock guard(locks_[i]);
      std::ranges::for_each(buckets_[i], func);
    }
  }

  // for_each_parallel executes func in parallel on each element using a
  // WorkerPool.
  template<typename F>
  void for_each_parallel(F func, size_t width = kDefaultParallelRangeWidth,
                         WorkerPool &pool = WorkerPool::get_default_pool())
  {
    assert(locks_.size() == buckets_.size());

    pool.for_each_parallel(
        std::views::zip(locks_, buckets_),
        [func](std::pair<lock_type &, Map &> arg) {
          std::scoped_lock guard(std::get<0>(arg));
          for (value_type &v : std::get<1>(arg)) {
            func(v);
          }
        },
        width);
  }

  // unsafe_clear clears the map without locking the buckets.
  void unsafe_clear()
  {
    for (size_t i = 0; i < locks_.size(); ++i) {
      buckets_[i].clear();
    }
  }

private:
  // Note: Per the c++ standard,
  // [containers.requirements.unord.req.general],
  // pointers and references to elements of an unordered_map are NOT invalidated
  // when the map is rehashed.
  // https://timsong-cpp.github.io/cppwp/n4950/unord.req#general-9
  using Map = std::unordered_map<Key, T, Hash, KeyEqual, Allocator>;

  Hash hash_fn_ = Hash();

  mutable std::vector<lock_type> locks_;
  std::vector<Map> buckets_;

  // get_bucket returns the bucket and its associated exclusive lock.
  ATTRIBUTE_ALWAYS_INLINE std::pair<Map &, std::unique_lock<lock_type>>
  get_bucket(const Key &key)
  {
    assert(std::has_single_bit(locks_.size()));
    assert(locks_.size() == buckets_.size());

    size_t idx = hash_fn_(key) & (buckets_.size() - 1);
    return {buckets_[idx], std::unique_lock<lock_type>(locks_[idx])};
  }

  // get_bucket_shared returns the bucket and its associated shared lock.
  ATTRIBUTE_ALWAYS_INLINE
  std::pair<const Map &, std::shared_lock<lock_type>>
  get_bucket_shared(const Key &key) const requires shared_mutex_like<lock_type>
  {
    assert(std::has_single_bit(locks_.size()));
    assert(locks_.size() == buckets_.size());

    size_t idx = hash_fn_(key) & (buckets_.size() - 1);
    return {buckets_[idx], std::shared_lock<lock_type>(locks_[idx])};
  }

  ATTRIBUTE_ALWAYS_INLINE Map &get_bucket(const Key &key,
                                          const std::unique_lock<lock_type> &ul)
  {
    assert(ul.owns_lock());
    assert(std::has_single_bit(locks_.size()));
    assert(locks_.size() == buckets_.size());
    size_t idx = hash_fn_(key) & (buckets_.size() - 1);
    assert(ul.mutex() == std::addressof(locks_[idx]));
    return buckets_[idx];
  }
};

} // namespace hogd
