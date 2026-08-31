#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "hogd/base/types.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/crypto/crypto.h"
#include "types.hh"

namespace hogd {

namespace detail {

// A fixed-size set of accounts sharing one bucket. A full bucket gives up its
// oldest entry: forgetting a change invalidates everything older than it, so
// dropping the oldest costs the least.
//
// Not synchronized; callers hold the [SynchronizedMap] bucket lock.
struct KeyChangeBucket {
  constexpr static size_t kBucketSize = 8;

  struct Entry {
    ContractAddress address = {};
    BlockHeight height = 0;
    bool occupied = false;
  };

  std::optional<BlockHeight> find(const ContractAddress &address) const
  {
    for (const Entry &entry : entries) {
      if (entry.occupied && entry.address == address) {
        return entry.height;
      }
    }
    return std::nullopt;
  }

  // Returns any height forgotten to make room.
  std::optional<BlockHeight> insert(const ContractAddress &address,
                                    BlockHeight height)
  {
    Entry *oldest = nullptr;
    for (Entry &entry : entries) {
      if (!entry.occupied) {
        entry.address = address;
        entry.height = height;
        entry.occupied = true;
        return std::nullopt;
      }
      if (entry.address == address) {
        entry.height = height;
        return std::nullopt;
      }
      if (oldest == nullptr || entry.height < oldest->height) {
        oldest = &entry;
      }
    }
    const BlockHeight forgotten = oldest->height;
    oldest->address = address;
    oldest->height = height;
    return forgotten;
  }

  std::array<Entry, kBucketSize> entries = {};
};

} // namespace detail

// Bounded record of when accounts last had their auth data written, for
// checking whether something derived from an account's keys is still good.
//
// Being bounded, changes can be forgotten. [forgotten_height] is the newest
// one dropped; nothing at or below it counts as current, since a forgotten
// change cannot be newer than that. It is -1, below every real height, while
// nothing has been dropped, which is where ordinary traffic leaves it.
//
// Per-account rather than a single global bar: anyone can rotate their own
// keys, so a global rule would let one account invalidate everyone's.
//
// [still_current] answers false when unsure, so callers must read that as
// "check again" rather than as a statement about the account.
class RecentKeyChanges {
public:
  // 1M buckets of 8 is 8M tracked accounts, and roughly 430MB if every one
  // of them is used. Buckets are materialized as accounts land in them, so
  // the resident cost tracks accounts that have actually written auth data
  // rather than the ceiling.
  explicit RecentKeyChanges(size_t num_buckets = 1 << 20)
    : num_buckets_(num_buckets)
  {}

  void note(const ContractAddress &address, BlockHeight height)
  {
    // Raise the bar while still holding the bucket lock. A change dropped
    // from the bucket is accounted for by nothing until the bar covers it,
    // and a reader in between would call a stale conclusion current.
    auto [bucket, bucket_lock] = buckets_[bucket_key(address)];
    const std::optional<BlockHeight> forgotten = bucket.insert(address, height);
    if (!forgotten.has_value()) {
      return;
    }
    BlockHeight bar = forgotten_height_.load(std::memory_order_relaxed);
    while (
        bar < *forgotten &&
        !forgotten_height_.compare_exchange_weak(bar, *forgotten,
                                                 std::memory_order_relaxed)) {
    }
  }

  // True when nothing here invalidates a conclusion drawn about [address] at
  // [height].
  bool still_current(const ContractAddress &address, BlockHeight height)
  {
    if (height <= forgotten_height_.load(std::memory_order_relaxed)) {
      return false;
    }
    auto [bucket, bucket_lock] = buckets_.find(bucket_key(address));
    if (bucket == nullptr) {
      return true;
    }
    const std::optional<BlockHeight> changed_at = bucket->find(address);
    return !changed_at.has_value() || height > *changed_at;
  }

  // Newest change dropped for want of room; -1 while none has been.
  BlockHeight forgotten_height() const
  {
    return forgotten_height_.load(std::memory_order_relaxed);
  }

private:
  uint64_t bucket_key(const ContractAddress &address) const
  {
    // Keyed per process, so an address's bucket cannot be worked out offline
    // and ground into. Addresses are chosen by whoever deploys them.
    return SipHash24Hasher{}(address) % num_buckets_;
  }

  size_t num_buckets_;
  SynchronizedMap<uint64_t, detail::KeyChangeBucket> buckets_;
  std::atomic<BlockHeight> forgotten_height_ = -1;
};

} // namespace hogd
