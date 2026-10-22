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
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <x23/corosync.h>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
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
//
// With x23::CoroLock, co_await operations and supply a caller-owned guard
// when retaining access to an element (CoroGuard cannot be moved):
// x23::CoroGuard guard;
// auto *val = co_await map.find(key, guard);
// guard.reset();
// co_await map.remove(key);
// To reuse an acquired guard, pass std::as_const(guard) to find/remove, or
// pass it after the value to insert(key, value, guard).
template<class Key, class T, class Hash = std::hash<Key>,
         class KeyEqual = std::equal_to<Key>,
         class Allocator = std::allocator<std::pair<const Key, T>>,
         class Lock = std::mutex>
requires (mutex_like<Lock> || std::same_as<Lock, x23::CoroLock>)
class SynchronizedMap {
public:
  // type definitions
  using key_type = Key;
  using map_type = T;
  using lock_type = Lock;
  static constexpr bool kCoro = std::same_as<Lock, x23::CoroLock>;
  using guard_type =
      std::conditional_t<kCoro, x23::CoroGuard, std::unique_lock<lock_type>>;
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
      buckets_[bucket_index(arg.first)].insert(arg);
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
      buckets_[bucket_index(arg.first)].insert(arg);
    });
  }
  ~SynchronizedMap() = default;

  // disable copy and support move.
  SynchronizedMap(const SynchronizedMap &map) = delete;
  SynchronizedMap &operator=(const SynchronizedMap &map) = delete;
  SynchronizedMap(SynchronizedMap &&map) noexcept = default;
  SynchronizedMap &operator=(SynchronizedMap &&map) noexcept = default;

  std::pair<T &, guard_type> operator[](const Key &key) requires (!kCoro)
  {
    auto [map, ul] = get_bucket(key);
    return {map[key], std::move(ul)};
  }

  // find with exclusive lock (for when you need to modify the value)
  std::pair<T *, guard_type> find(const Key &key) requires (!kCoro)
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

  T *find(const Key &key, const guard_type &ul)
  {
    auto &map = get_bucket(key, ul);
    auto it = map.find(key);
    if (it == map.end())
      return nullptr;
    return &it->second;
  }

  template<typename... Args>
  requires (!kCoro && std::constructible_from<T, Args...>)
  std::pair<T *, guard_type> insert(const Key &k, Args &&...args)
  {
    auto [map, ul] = get_bucket(k);
    auto [it, inserted] =
        map.emplace(std::piecewise_construct, std::forward_as_tuple(k),
                    std::forward_as_tuple(std::forward<Args>(args)...));
    if (!inserted)
      return {nullptr, std::move(ul)};
    return {&it->second, std::move(ul)};
  }

  std::pair<T *, guard_type> insert(const Key &k, const T &obj)
      requires (!kCoro)
  {
    auto [map, ul] = get_bucket(k);
    auto [it, inserted] = map.insert({k, obj});
    if (!inserted)
      return {nullptr, std::move(ul)};
    return {&it->second, std::move(ul)};
  }

  T *insert(const Key &key, T &&obj, const guard_type &ul)
  {
    auto [it, inserted] = get_bucket(key, ul).insert({key, std::move(obj)});
    if (!inserted)
      return nullptr;
    return &it->second;
  }

  std::tuple<T *, guard_type, bool> find_or_insert(const Key &k, const T &obj)
      requires (!kCoro)
  {
    auto [map, ul] = get_bucket(k);
    auto [it, inserted] = map.insert({k, obj});
    return {&it->second, std::move(ul), inserted};
  }

  std::pair<T *, guard_type> insert_or_assign(const Key &k, T &&obj)
      requires (!kCoro)
  {
    auto [map, ul] = get_bucket(k);
    auto [it, inserted] = map.insert_or_assign(k, std::move(obj));
    return {&it->second, std::move(ul)};
  }

  size_t remove(const Key &key) requires (!kCoro)
  {
    auto [map, ul] = get_bucket(key);
    return map.erase(key);
  }

  size_t remove(const Key &key, const guard_type &ul)
  {
    return get_bucket(key, ul).erase(key);
  }

  size_t size() requires (!kCoro)
  {
    assert(locks_.size() == buckets_.size());

    size_t ret = 0;
    for (size_t i = 0; i < locks_.size(); ++i) {
      std::scoped_lock guard(locks_[i]);
      ret += buckets_[i].size();
    }
    return ret;
  }

  void clear() requires (!kCoro)
  {
    assert(locks_.size() == buckets_.size());

    for (size_t i = 0; i < locks_.size(); ++i) {
      std::scoped_lock guard(locks_[i]);
      buckets_[i].clear();
    }
  }

  // for_each executes func on each element.
  template<typename F> void for_each(F func) requires (!kCoro)
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
      requires (!kCoro)
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

  auto operator[](const Key &key, x23::CoroGuard &guard) requires kCoro
  {
    return with_bucket(key, guard,
                       [&key](auto &map) -> T & { return map[key]; });
  }

  auto find(const Key &key, x23::CoroGuard &guard) requires kCoro
  {
    return with_bucket(key, guard, [&key](auto &map) -> T * {
      auto it = map.find(key);
      return it == map.end() ? nullptr : &it->second;
    });
  }

  template<typename... Args>
  requires (kCoro && std::constructible_from<T, Args...>)
  auto insert(const Key &key, x23::CoroGuard &guard, Args &&...args)
  {
    return with_bucket(key, guard, [&](auto &map) -> T * {
      auto [it, inserted] = map.try_emplace(key, std::forward<Args>(args)...);
      return inserted ? &it->second : nullptr;
    });
  }

  auto find_or_insert(const Key &key, const T &obj, x23::CoroGuard &guard)
      requires kCoro
  {
    return with_bucket(key, guard, [&](auto &map) {
      auto [it, inserted] = map.insert({key, obj});
      return std::pair{&it->second, inserted};
    });
  }

  auto insert_or_assign(const Key &key, T &&obj, x23::CoroGuard &guard)
      requires kCoro
  {
    return with_bucket(key, guard, [&](auto &map) {
      return &map.insert_or_assign(key, std::move(obj)).first->second;
    });
  }

  x23::Async<size_t> remove(const Key &key) requires kCoro
  {
    x23::CoroGuard guard;
    co_return co_await with_bucket(key, guard, [&key](auto &map) {
      return map.erase(key);
    });
  }

  x23::Async<size_t> size() requires kCoro
  {
    size_t result = 0;
    co_await for_each_bucket([&](auto &map) { result += map.size(); });
    co_return result;
  }

  auto clear() requires kCoro
  {
    return for_each_bucket([](auto &map) { map.clear(); });
  }

  template<typename F> auto for_each(F func) requires kCoro
  {
    return for_each_bucket([func = std::move(func)](auto &map) {
      std::ranges::for_each(map, func);
    });
  }

  template<typename F>
  x23::Async<void>
  for_each_parallel(F func, size_t width = kDefaultParallelRangeWidth,
                    WorkerPool &pool = WorkerPool::get_default_pool())
      requires kCoro
  {
    CHECK_GT(width, size_t{0});
    auto chunk = [this, &func, &pool, width](size_t start) -> x23::Async<void> {
      co_await OnWorker{pool};
      for (size_t i = start;
           i < start + std::min(width, buckets_.size() - start); ++i) {
        x23::CoroGuard guard;
        co_await guard(locks_[i]);
        std::ranges::for_each(buckets_[i], func);
      }
    };
    std::vector<x23::Async<void>> jobs;
    jobs.reserve(divide_up(buckets_.size(), width));
    for (size_t i = 0; i < buckets_.size(); i += width)
      jobs.push_back(chunk(i));
    for (auto &job : jobs)
      co_await job;
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

  template<typename F>
  auto with_bucket(const Key &key, x23::CoroGuard &guard, F func) requires kCoro
  {
    size_t idx = bucket_index(key);
    return x23::MapAwait{[this, idx, func = std::move(func)]() mutable
                             -> decltype(auto) { return func(buckets_[idx]); },
                         guard(locks_[idx])};
  }

  template<typename F> x23::Async<void> for_each_bucket(F func) requires kCoro
  {
    for (size_t i = 0; i < buckets_.size(); ++i) {
      x23::CoroGuard guard;
      co_await guard(locks_[i]);
      func(buckets_[i]);
    }
  }

  struct OnWorker {
    WorkerPool &pool;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) const
    {
      pool.submit_detached([h] { h.resume(); });
    }
    void await_resume() const noexcept {}
  };

  // get_bucket returns the bucket and its associated exclusive lock.
  ATTRIBUTE_ALWAYS_INLINE std::pair<Map &, guard_type>
  get_bucket(const Key &key) requires (!kCoro)
  {
    size_t idx = bucket_index(key);
    return {buckets_[idx], guard_type(locks_[idx])};
  }

  // get_bucket_shared returns the bucket and its associated shared lock.
  ATTRIBUTE_ALWAYS_INLINE
  std::pair<const Map &, std::shared_lock<lock_type>>
  get_bucket_shared(const Key &key) const requires shared_mutex_like<lock_type>
  {
    size_t idx = bucket_index(key);
    return {buckets_[idx], std::shared_lock<lock_type>(locks_[idx])};
  }

  ATTRIBUTE_ALWAYS_INLINE Map &get_bucket(const Key &key,
                                          const guard_type &guard)
  {
    size_t idx = bucket_index(key);
    const lock_type *lock;
    if constexpr (kCoro)
      lock = guard.lkp_;
    else {
      assert(guard.owns_lock());
      lock = guard.mutex();
    }
    if (lock != std::addressof(locks_[idx])) [[unlikely]]
      panic("Wrong lock.");
    return buckets_[idx];
  }

  size_t bucket_index(const Key &key) const
  {
    assert(std::has_single_bit(locks_.size()));
    assert(locks_.size() == buckets_.size());
    return hash_fn_(key) & (buckets_.size() - 1);
  }
};

} // namespace hogd
