#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/metrics.h"
#include "types.hh"
#include "x23/testlock.h"

namespace hogd {

namespace detail {
// Follows a similar eviction policy as the AuthDataCacheBucket.
// However, does not need (or maintain) the same update consistency policy,
// so the implementation is simpler and requires less metadata.
class ModuleCacheBucket {
public:
  constexpr static size_t kBucketSize = 16;

  void put(const SHA256Hash &hash, std::shared_ptr<void *> entry)
  {
    std::lock_guard lock(lock_);
    auto *it = std::ranges::find_if(cache_, [hash](const auto &item) {
      return item.hash == hash;
    });
    if (it != cache_.end()) {
      it->read = true;
      return;
    }

    for (;;) {
      if (cache_[clock_hand_].read) {
        cache_[clock_hand_].read = false;
        clock_hand_ = (clock_hand_ + 1) % kBucketSize;
        continue;
      }

      break;
    }
    // evict what was there before
    cache_[clock_hand_].hash = hash;
    cache_[clock_hand_].value = std::move(entry);
    cache_[clock_hand_].read = true;
  }

  std::shared_ptr<void *> get(const SHA256Hash &hash)
  {
    std::lock_guard lock(lock_);
    auto *it = std::ranges::find_if(cache_, [hash](const auto &item) {
      return item.hash == hash;
    });
    if (it == cache_.end()) {
      return nullptr;
    }
    it->read = true;
    return it->value;
  }

private:
  struct BucketEntry {
    SHA256Hash hash{};
    std::shared_ptr<void *> value = nullptr;
    bool read = false;
  };

  x23::TestLock lock_;
  size_t clock_hand_ = 0;
  std::array<BucketEntry, kBucketSize> cache_;
};

} // namespace detail

class ModuleCache {
public:
  ModuleCache(size_t bucket_count) : buckets_(bucket_count) {}

  void put(const SHA256Hash &hash, std::shared_ptr<void *> entry)
  {
    buckets_[get_bucket_index(hash)].put(hash, std::move(entry));
  }

  std::shared_ptr<void *> get(SHA256Hash const &hash)
  {
    auto result = buckets_[get_bucket_index(hash)].get(hash);
    if (result != nullptr) {
      HogexecMetrics::mark_module_cache_hit();
    }
    else {
      HogexecMetrics::mark_module_cache_miss();
    }
    return result;
  }

private:
  uint16_t get_bucket_index(const SHA256Hash &hash) const
  {
    static SipHash24 hasher;
    return hasher(std::as_bytes(std::span(hash))) % buckets_.size();
  }
  std::vector<detail::ModuleCacheBucket> buckets_;
};

} // namespace hogd
