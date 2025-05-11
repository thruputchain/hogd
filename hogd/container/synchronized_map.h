// synchronized_map.h - provides a bucket hash table that exposes locking

#pragma once

#include <algorithm>
#include <concepts>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/worker_pool.h"

namespace hogd {

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
         class Allocator = std::allocator<std::pair<const Key, T>>>
class SynchronizedMap {
public:
  // type definitions
  using key_type = Key;
  using map_type = T;
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

  std::pair<T &, std::unique_lock<std::mutex>> operator[](const Key &key)
  {
    auto [map, ul] = get_bucket(key);
    return {map[key], std::move(ul)};
  }

  std::pair<T *, std::unique_lock<std::mutex>> find(const Key &key)
  {
    auto [map, ul] = get_bucket(key);
    auto it = map.find(key);
    if (it == map.end()) {
      ul.unlock();
      return {nullptr, std::move(ul)};
    }
    return {&it->second, std::move(ul)};
  }

  std::pair<T *, std::unique_lock<std::mutex>> insert(const Key &k, T &&obj)
  {
    auto [map, ul] = get_bucket(k);
    auto [it, inserted] = map.insert({k, std::move(obj)});
    if (!inserted) {
      ul.unlock();
      return {nullptr, std::move(ul)};
    }
    return {&it->second, std::move(ul)};
  }

  std::pair<T *, std::unique_lock<std::mutex>> insert(const Key &k,
                                                      const T &obj)
  {
    auto [map, ul] = get_bucket(k);
    auto [it, inserted] = map.insert({k, obj});
    if (!inserted) {
      ul.unlock();
      return {nullptr, std::move(ul)};
    }
    return {&it->second, std::move(ul)};
  }

  std::pair<T &, std::unique_lock<std::mutex>> insert_or_assign(const Key &k,
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

  size_t remove(const Key &key, const std::unique_lock<std::mutex> &ul)
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
        [func](std::pair<std::mutex &, Map &> arg) {
          std::scoped_lock guard(std::get<0>(arg));
          for (value_type &v : std::get<1>(arg)) {
            func(v);
          }
        },
        width);
  }

private:
  using Map = std::unordered_map<Key, T, Hash, KeyEqual, Allocator>;

  Hash hash_fn_ = Hash();

  std::vector<std::mutex> locks_;
  std::vector<Map> buckets_;

  // get_bucket returns the bucket and its associated lock for a given key.
  ATTRIBUTE_ALWAYS_INLINE std::pair<Map &, std::unique_lock<std::mutex>>
  get_bucket(const Key &key)
  {
    assert(std::has_single_bit(locks_.size()));
    assert(locks_.size() == buckets_.size());

    size_t idx = hash_fn_(key) & (buckets_.size() - 1);
    return {buckets_[idx], std::unique_lock<std::mutex>(locks_[idx])};
  }
};

} // namespace hogd
