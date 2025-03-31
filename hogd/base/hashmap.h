#pragma once

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "x23/cleanup.h"

namespace hogd {

// Types in the hashmap must be movable and must be able to reconstruct their own key.
template<typename T>
concept hash_map_object = std::movable<T> && requires(T object) {
  { object.get_key() } -> std::same_as<std::span<const std::byte>>;
};

namespace detail {

struct Hasher {
  static uint64_t hash(std::span<const std::byte> key)
  {
    return SipHash24::get_default()(key) & std::numeric_limits<uint64_t>::max();
  }
};

// BucketEntry type abstracts the required operations on a bucket,
// to allow alternative implementations of each bucket.
// Required methods are emplace(), erase(), and find_element().
template<typename BackingType> struct BucketEntry;

// Vector implementation
template<hash_map_object T>
struct BucketEntry<std::vector<T>> : private std::vector<T> {

  template<typename... Args> auto &emplace(Args &&...args)
  {
    return std::vector<T>::emplace_back(std::forward<Args>(args)...);
  }

  bool erase(std::span<const std::byte> key)
  {
    for (auto it = this->begin(); it != this->end(); ++it) {
      if (std::ranges::equal(it->get_key(), key)) {
        *it = std::move(this->back());
        this->pop_back();
        return true;
      }
    }
    return false;
  }

  auto *find_element(this auto &self, std::span<const std::byte> key)
  {
    for (auto &elt : self) {
      if (std::ranges::equal(elt.get_key(), key)) {
        return &elt;
      }
    }
    return static_cast<decltype(&*self.begin())>(nullptr);
  }
};

template<hash_map_object T> struct CustomHash {
  using is_transparent = void;
  static std::size_t operator()(T const &obj)
  {
    return Hasher::hash(obj.get_key());
  }
  static std::size_t operator()(std::span<const std::byte> key)
  {
    return Hasher::hash(key);
  }
};

template<hash_map_object T> struct CustomEqual {
  using is_transparent = void;

  using key_t = std::span<const std::byte>;
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

// Wrapper around object inside unordered set.
// It's required that the object cannot be modified in a way
// that changes the object's key.  This object ensures
// that we can still modify the object, without having to
// use a (potentially error-prone) const_cast.
template<hash_map_object T> struct MutableWrapper {
  template<typename... Args>
  MutableWrapper(Args &&...args) : base_(std::forward<Args>(args)...)
  {}
  mutable T base_;

  std::span<const std::byte> get_key() const { return base_.get_key(); }
};

template<hash_map_object T> struct UnorderedSetMarker {};

// Unordered set implementation, using cpp20 transparent comparators.
template<hash_map_object T>
struct BucketEntry<UnorderedSetMarker<T>>
  : private std::unordered_set<MutableWrapper<T>, CustomHash<MutableWrapper<T>>,
                               CustomEqual<MutableWrapper<T>>> {

  using self_type = BucketEntry<UnorderedSetMarker<T>>;

  using base_type =
      std::unordered_set<MutableWrapper<T>, CustomHash<MutableWrapper<T>>,
                         CustomEqual<MutableWrapper<T>>>;
  template<typename... Args> auto &emplace(Args &&...args)
  {
    auto [it, _] = base_type::emplace(std::forward<Args>(args)...);
    return it->base_;
  }

  bool erase(std::span<const std::byte> key)
  {
    // cpp23 feature: p2077r3
#if __cpp_lib_associative_heterogeneous_erasure >= 202110L
    auto res = bucket->erase(key);
    assert(res == 0 || res == 1);
    return res == 1;
#else
    // transparent erase not yet supported
    auto it = base_type::find(key);
    if (it != base_type::end()) {
      base_type::erase(it);
      return true;
    }
    return false;
#endif
  }

  auto *find_element(this auto &self, std::span<const std::byte> key)
  {
    auto it = self.find(key);
    if (it == self.end()) {
      return static_cast<decltype(&(it->base_))>(nullptr);
    }
    return &(it->base_);
  }
};

} // namespace detail

// This version implements both a vector option for an individual bucket
// and an unordered_set option.
template<hash_map_object T>
using vector_bucket_t = detail::BucketEntry<std::vector<T>>;
template<hash_map_object T>
using set_bucket_t = detail::BucketEntry<detail::UnorderedSetMarker<T>>;


// Hashmap designed for concurrent accesses.  Elements are stored
// in separate buckets, guarded by bucket locks.
//
// Objects contained need not be trivially-copyable,
// and are not stored as bytespans (in particular, they could contain
// pointers to externally allocated memory).  This version is NOT designed
// for persisting data to disk.
//
// Types contained must be movable (in order to implement bucket resizing)
// but a lock will be held while movement is occuring.
//
// KEY REQUIREMENT:
// Modifying an element CANNOT change its key.
//
// TODO(geoff): investigate whether this should also use a custom allocator,
// and investigate performance of different bucket implementations on various
// workloads.
template<hash_map_object T, typename BucketEntryT = vector_bucket_t<T>>
class HashMap {
public:
  using hash_t = uint64_t;
  using key_t = std::span<const std::byte>;

  HashMap(int lg_bucket_count)
    : buckets_(1UL << lg_bucket_count), mutexes_(1UL << lg_bucket_count)
  {
    assert(lg_bucket_count <= 64);
  }

  // Performs the supplied lambda on the object at the supplied key,
  // if the object exists.  Lambda is not invoked if the object does not exist.
  //
  // The lambda can modify the object.  HOWEVER,
  // the object's key, which can be computed from the object during lookups,
  // must NEVER be modified by one of these lambdas.
  //
  // Allowed but bad practice to block within the invoked call.
  // Returns nullopt if the object is not found.
  template<typename Func,
           typename Ret = std::invoke_result_t<std::decay_t<Func>, T &>>
  requires std::is_invocable_r_v<Ret, Func, T &>
  auto modify_element(key_t key, Func &&lambda)
  {
    auto [lk, bucket] = acquire_bucket(key);
    if (auto *ptr = bucket->find_element(key)) {
#if DEBUG
      x23::Defer d(
          [ptr, key]() { assert(std::ranges::equal(ptr->get_key(), key)); });
#endif
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

  // Creates an object if does not already exist.
  // Then applies the supplied lambda to the (new or preexisting) object.
  // The supplied lambda MUST NOT modify the object's key.
  template<typename Func,
           typename Ret = std::invoke_result_t<std::decay_t<Func>, T &>,
           typename... Args>
  requires std::is_invocable_r_v<Ret, Func, T &>
  auto emplace_and_modify(key_t key, Func &&lambda, Args &&...args)
  {
    auto [lk, bucket] = acquire_bucket(key);

    if (auto *ptr = bucket->find_element(key)) {
#if DEBUG
      x23::Defer d(
          [ptr, key]() { assert(std::ranges::equal(ptr->get_key(), key)); });
#endif
      return lambda(*ptr);
    }
    return lambda(bucket->emplace(std::forward<Args>(args)...));
  }

  // returns true if a new object is constructed.
  // Warning: If you insert an element and then later, get_key() returns
  // a different result than the key under which it was inserted,
  // the element might be lost.
  template<typename... Args> bool emplace(key_t key, Args &&...args)
  {
    auto [lk, bucket] = acquire_bucket(key);
    if (auto *ptr = bucket->find_element(key)) {
      return false;
    }

    [[maybe_unused]] auto &new_element =
        bucket->emplace(std::forward<Args>(args)...);
    assert(std::ranges::equal(new_element.get_key(), key));
    return true;
  }

  // returns true if an element is erased
  bool erase(key_t key)
  {
    auto [lk, bucket] = acquire_bucket(key);
    return bucket->erase(key);
  }

  static uint64_t hash(key_t key) { return detail::Hasher::hash(key); }

private:
  std::vector<BucketEntryT> buckets_;
  std::vector<std::mutex> mutexes_;

  std::pair<std::unique_lock<std::mutex>, BucketEntryT *>
  acquire_bucket(key_t key)
  {
    uint64_t h = hash(key);
    std::unique_lock ul(get_mutex(h));

    size_t idx = (buckets_.size() - 1) & h;
    return {std::move(ul), &buckets_[idx]};
  }

  std::mutex &get_mutex(uint64_t hash)
  {
    uint64_t mask = mutexes_.size() - 1;
    // assert mutexes_.size() is power of 2
    assert((mutexes_.size() & mask) == 0);
    return mutexes_[hash & mask];
  }
};

} // namespace hogd
