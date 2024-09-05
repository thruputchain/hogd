#pragma once

/*! \file threadlocal_cache.h

Maintain a concurrently-accessible threadlocal storage,
without explicitly using pthread tls keys.
*/

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

#include "hogd/base/non_movable.h"

namespace hogd {

// Stores a unique thread_local identifier (uint32_t),
// with the guarantee that ids are allocated sequentially, starting from 0
// Call get() to access id.
class ThreadlocalIdentifier {
public:
  static uint32_t get() { return kTid; }

  ThreadlocalIdentifier() = delete;

private:
  static uint32_t initialize()
  {
    uint32_t val = tl_initializer_.fetch_add(1, std::memory_order_relaxed);

    if (val == UINT32_MAX) {
      // initializer has overflowed
      throw std::runtime_error("overflow ThreadlocalIdentifier");
    }
    return val;
  }

  inline static std::atomic<uint32_t> tl_initializer_ = 0;
  inline static thread_local const uint32_t kTid = initialize();
};

/*! Threadlocal object cache.

Using ThreadlocalIdentifier is safe in any config, since each thread's id
is unique.  Downside is that more threads running could consume identifiers,
and so at some point, we'll need to boost CACHE_SIZE.
One could, in theory, swap the array out with a (threadsafe) map,
but that substantially increases access overhead.
*/

template<typename ValueType, uint32_t CacheSize>
class ThreadlocalCache : NonMovableOrCopyable {
public:
  using cache_array_t = std::array<std::optional<ValueType>, CacheSize>;

  ThreadlocalCache() : objects_() {}

  //! Get current thread's cached object.
  //! Input constructor arguments for cached object, in the case
  //! that this is the first access (and so thread's cache is empty).
  template<typename... ConstructorArgs> ValueType &get(ConstructorArgs &...args)
  {
    uint32_t idx = ThreadlocalIdentifier::get();
    if (idx >= CacheSize) {
      throw std::runtime_error("invalid tlcache access!");
    }
    if (!objects_[idx]) {
      objects_[idx].emplace(args...);
    }
    return *objects_[idx]; // NOLINT
  }

  //! At times it is useful to export the list of cached objects
  //! all at once.
  auto &get_objects(this auto &self) { return self.objects_; }

  void clear()
  {
    for (uint32_t i = 0; i < CacheSize; i++) {
      objects_[i] = std::nullopt;
    }
  }

private:
  cache_array_t objects_;
};

} // namespace hogd
