#pragma once

#include <atomic>
#include <concepts>
#include <cstdint>
#include <format>
#include <mutex>
#include <unordered_set>

#include "hogd/base/worker_pool.h"
#include "hogd/crypto/crypto.h"

namespace hogd {

// Hashmap designed for concurrent access.
//
// Objects contained need not be trivially-copyable,
// and are not stored as bytespans (in particular, they could contain
// pointers to externally allocated memory).  This version is NOT designed
// for persisting data to disk.
// Types contained must be movable (in order to implement vector resizing)
// but a lock will be held while movement is occuring.
//
// TODO(geoff): investigate whether this should also use a custom allocator
template<typename T>
concept hash_map_object = std::movable<T> && requires(T object) {
  { object.get_key() } -> std::same_as<std::span<const std::byte>>;
};

#define VECTOR 1

template<hash_map_object T> class HashMap {
public:
  using hash_t = uint64_t;
  using key_t = std::span<const std::byte>;

  struct CustomHash {
    using is_transparent = void;
    static std::size_t operator()(T const &obj) { return hash(obj.get_key()); }
    static std::size_t operator()(key_t key) { return hash(key); }
  };

  struct CustomEqual {
    using is_transparent = void;
    static bool operator()(T const &lhs, T const &rhs)
    {
      return std::ranges::equal(lhs.get_key(), rhs.get_key());
    }
    static bool operator()(key_t lhs, key_t rhs)
    {
      return std::ranges::equal(lhs, rhs);
    }
    static bool operator()(T const &lhs, key_t rhs)
    {
      return std::ranges::equal(lhs.get_key(), rhs);
    }
    static bool operator()(key_t lhs, T const &rhs)
    {
      return std::ranges::equal(lhs, rhs.get_key());
    }
  };

#if VECTOR
  using bucket_entry_t = std::vector<T>;
#else
  using bucket_entry_t = std::unordered_set<T, CustomHash, CustomEqual>;
#endif

  HashMap(int lg_bucket_count)
    : buckets_(1UL << lg_bucket_count), mutexes_(1UL << lg_bucket_count)
  {
    assert(lg_bucket_count <= 64);
  }

  static std::string key_to_str(key_t key)
  {
    std::span<const uint8_t> s = {reinterpret_cast<const uint8_t *>(key.data()),
                                  key.size()};

    std::string out = "[";
    for (auto i : s) {
      out += std::format("{:02x}", i);
    }
    return out + "]";
  }

  // Technically allowed but bad practice to block within the invoked call.
  // Returns nullopt if the object is not found.
  template<typename Func,
           typename Ret = std::invoke_result_t<std::decay_t<Func>, T &>>
  requires std::is_invocable_r_v<Ret, Func, T &>
  auto modify_element(key_t key, Func &&lambda)
  {
    auto [lk, bucket] = acquire_bucket(key);

    if (T *ptr = find_element(key, *bucket)) {
      if constexpr (std::is_void_v<Ret>) {
        lambda(*ptr);
        return true;
      }
      else {
        return std::make_optional<Ret>(lambda(*ptr));
      }
    }
    if constexpr (std::is_void_v<Ret>) {
      return false;
    }
    else {
      constexpr static std::optional<Ret> kOut = std::nullopt;
      return kOut;
    }
  }

  // In most use cases (i.e. in storage view use case), this function is applied
  // to all elements when there's already a guarantee that the map is not being
  // modified.  But it does not particularly hurt to acquire the mutex.
  // This method CANNOT modify the key of the object.  That would break the
  // unordered_set.
  template<typename Func> requires std::is_invocable_v<Func, T &>
  void map_apply(Func lambda)
  {
    auto &pool = WorkerPool::get_default_pool();
    std::vector<std::future<void>> futures;
    for (size_t i = 0; i < buckets_.size(); ++i) {
      auto &bucket = buckets_[i];
      auto &mutex = mutexes_[i];
      futures.push_back(pool.submit([&bucket, &mutex, lambda]() {
        std::unique_lock lk(mutex);
        for (auto &elt : bucket) {
          lambda(const_cast<T &>(elt));
        }
      }));
    }
    for (auto &future : futures) {
      future.get();
    }
  }

  // returns true if a new object is constructed.
  // Warning: If you insert an element and then later, get_key() returns
  // a different result than the key under which it was inserted,
  // the element might be lost.
  template<typename... Args> bool emplace(key_t key, Args &&...args)
  {
    auto [lk, bucket] = acquire_bucket(key);
    if (T *ptr = find_element(key, *bucket)) {
      return false;
    }

#if VECTOR
    bucket->emplace_back(std::forward<Args>(args)...);
#else
    bucket->emplace(std::forward<Args>(args)...);
#endif
    return true;
  }

  // returns true if an element is erased
  bool erase(key_t key)
  {
    auto [lk, bucket] = acquire_bucket(key);

#if !VECTOR
    // transparent erase not yet supported
    // auto res = bucket->erase(key);
    auto it = bucket->find(key);
    if (it != bucket->end()) {
      bucket->erase(it);
      return true;
    }
    return false;
  }
#else
    for (auto it = bucket->begin(); it != bucket->end(); ++it) {
      if (std::ranges::equal(it->get_key(), key)) {
        *it = std::move(bucket->back());
        bucket->pop_back();
        return true;
      }
    }
    return false;
  }
#endif

private:
  std::vector<bucket_entry_t> buckets_;
  std::vector<std::mutex> mutexes_;
  std::atomic<size_t> expanded_idx_;
  size_t old_size_;

  std::pair<std::unique_lock<std::mutex>, bucket_entry_t *>
  acquire_bucket(key_t key);
  std::mutex &get_mutex(uint64_t hash);

  static uint64_t hash(key_t key)
  {
    return SipHash24::get_default()(key) & std::numeric_limits<uint64_t>::max();
  }
#if !VECTOR
  const T *find_element(key_t key, bucket_entry_t const &bucket) const
  {
    auto it = bucket.find(key);
    if (it != bucket.end()) {
      return &(*it);
    }
    return nullptr;
  }

  T *find_element(key_t key, bucket_entry_t &bucket)
  {
    auto it = bucket.find(key);
    if (it != bucket.end()) {
      return const_cast<T *>(&(*it));
    }
    return nullptr;
  }
#else

  T *find_element(key_t key, bucket_entry_t &bucket)
  {
    for (auto &elt : bucket) {
      if (std::ranges::equal(elt.get_key(), key)) {
        return &elt;
      }
    }
    return nullptr;
  }

#endif
};

template<hash_map_object T>
std::pair<std::unique_lock<std::mutex>, typename HashMap<T>::bucket_entry_t *>
HashMap<T>::acquire_bucket(key_t key)
{
  uint64_t h = hash(key);
  std::unique_lock ul(get_mutex(h));

  size_t idx = (buckets_.size() - 1) & h;
  return {std::move(ul), &buckets_[idx]};
}

template<hash_map_object T>
std::mutex &
HashMap<T>::get_mutex(uint64_t hash)
{
  uint64_t mask = mutexes_.size() - 1;
  // assert mutexes_.size() is power of 2
  assert((mutexes_.size() & mask) == 0);
  return mutexes_[hash & mask];
}

} // namespace hogd
