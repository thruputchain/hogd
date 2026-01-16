#pragma once

#include "address_auth.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "types.hh"
#include "x23/testlock.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace hogd {

namespace detail {

class AuthDataCacheBucket {
public:
  void
  put(const ContractAddress &address,
      std::shared_ptr<std::pair<BlockHeight, std::optional<AddressAuthData>>>
          auth_data)
  {
    CHECK_PTR(auth_data.get());
    std::lock_guard lock(lock_);

    if (auth_data->first < max_observed_height_) {
      return;
      // nothing to do: the update is from a block in the past.
      // It's possible that an update was placed for this address at a higher
      // height, and then evicted.
    }
    max_observed_height_ = std::max(max_observed_height_, auth_data->first);

    auto *it = std::ranges::find_if(cache_, [address](const auto &item) {
      return item.address == address;
    });
    if (it != cache_.end()) {
      // the update must be newer than the entry already present, so replace it
      if (it->auth_data) {
        assert(it->auth_data->first <= auth_data->first);
      }
      it->address = address;
      it->auth_data = std::move(auth_data);

      return;
    }

    // otherwise evict from the cache.
    // This implements a simple clock policy
    for (;;) {
      if (cache_[clock_hand_].read) {
        cache_[clock_hand_].read = false;
        clock_hand_ = (clock_hand_ + 1) % kBucketSize;
        continue;
      }

      // evict
      cache_[clock_hand_].address = address;
      cache_[clock_hand_].auth_data = std::move(auth_data);
      cache_[clock_hand_].read = false;
      clock_hand_ = (clock_hand_ + 1) % kBucketSize;
      break;
    }
  }

  std::shared_ptr<std::pair<BlockHeight, std::optional<AddressAuthData>>>
  get(ContractAddress const &address)
  {
    std::lock_guard lock(lock_);
    auto *it = std::ranges::find_if(cache_, [address](const auto &item) {
      return item.address == address;
    });
    if (it == cache_.end()) {
      return nullptr;
    }
    it->read = true;
    return it->auth_data;
  }

  void evict_all()
  {
    std::lock_guard lock(lock_);
    cache_.fill({});
    clock_hand_ = 0;
  }

private:
  constexpr static size_t kBucketSize = 16;
  struct BucketEntry {
    ContractAddress address = {};
    std::shared_ptr<std::pair<BlockHeight, std::optional<AddressAuthData>>>
        auth_data = nullptr;
    bool read = false;
  };

  x23::TestLock lock_;

  std::array<BucketEntry, kBucketSize> cache_;
  size_t clock_hand_ = 0;
  BlockHeight max_observed_height_ = 0;
};

} // namespace detail
class AuthDataCache {
public:
  AuthDataCache(int bucket_count) : buckets_(bucket_count) {}
  ~AuthDataCache() = default;

  // Caches both positive and negative lookups.
  void
  put(const ContractAddress &address,
      std::shared_ptr<std::pair<BlockHeight, std::optional<AddressAuthData>>>
          auth_data)
  {
    size_t bucket_index = get_bucket_index(address);
    buckets_[bucket_index].put(address, std::move(auth_data));
  }

  std::shared_ptr<std::pair<BlockHeight, std::optional<AddressAuthData>>>
  get(const ContractAddress &address)
  {
    return buckets_[get_bucket_index(address)].get(address);
  }

  void evict_all()
  {
    for (auto &bucket : buckets_) {
      bucket.evict_all();
    }
  }

private:
  size_t get_bucket_index(const ContractAddress &address) const
  {
    static SipHash24 hasher;
    return hasher(std::as_bytes(std::span(address))) % buckets_.size();
  }
  std::vector<detail::AuthDataCacheBucket> buckets_;
};

} // namespace hogd
