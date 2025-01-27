#pragma once

#include <atomic>
#include <concepts>
#include <cstdint>
#include <format>
#include <mutex>

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
//
// Loosely based on @abelay's design in
// https://github.com/groundhogchain/hogd/pull/67
// This version does not implement a size count, although nothing would prevent
// that.
template<typename T>
concept hash_map_object = std::movable<T> && requires(T object) {
  { object.get_key() } -> std::same_as<std::span<const std::byte>>;
};

template<hash_map_object T> class HashMap {
public:
  using hash_t = uint64_t;
  using key_t = std::span<const std::byte>;
  using bucket_entry_t = std::vector<T>;

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

    bucket->emplace_back(std::forward<Args>(args)...);
    return true;
  }

  // returns true if an element is erased
  bool erase(key_t key)
  {
    auto [lk, bucket] = acquire_bucket(key);
    for (auto it = bucket->begin(); it != bucket->end(); ++it) {
      if (std::ranges::equal(it->get_key(), key)) {
        *it = std::move(bucket->back());
        bucket->pop_back();
        return true;
      }
    }
    return false;
  }

private:
  std::vector<bucket_entry_t> buckets_;
  std::vector<std::mutex> mutexes_;
  std::atomic<size_t> expanded_idx_;
  size_t old_size_;

  std::pair<std::unique_lock<std::mutex>, bucket_entry_t *>
  acquire_bucket(key_t key);
  std::mutex &get_mutex(uint64_t hash);

  uint64_t hash(key_t key) const
  {
    return SipHash24::get_default()(key) & std::numeric_limits<uint64_t>::max();
  }

  static T *find_element(key_t key, bucket_entry_t &bucket)
  {
    for (auto &elt : bucket) {
      if (std::ranges::equal(elt.get_key(), key)) {
        return &elt;
      }
    }
    return nullptr;
  }
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
