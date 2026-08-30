#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "hogd/base/os.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/crypto/crypto.h"
#include "types.hh"

namespace hogd {

namespace detail {

// A fixed-size set of signings sharing one bucket key, evicting under a clock
// policy: a lookup sets the entry's read bit, and an insert advances the hand
// past set bits, clearing them, until it finds a slot to take. A signing that
// keeps being resubmitted survives; one submitted once does not.
//
// Not synchronized. Callers hold the [SynchronizedMap] bucket lock covering
// this value.
struct FailedSignatureBucket {
  constexpr static size_t kBucketSize = 16;

  struct Entry {
    SHA256Hash signed_tx_hash = {};
    bool occupied = false;
    bool read = false;
  };

  bool contains(const SHA256Hash &signed_tx_hash)
  {
    for (Entry &entry : entries) {
      if (entry.occupied && entry.signed_tx_hash == signed_tx_hash) {
        entry.read = true;
        return true;
      }
    }
    return false;
  }

  void insert(const SHA256Hash &signed_tx_hash)
  {
    for (Entry &entry : entries) {
      if (entry.occupied && entry.signed_tx_hash == signed_tx_hash) {
        entry.read = true;
        return;
      }
    }
    for (;;) {
      if (entries[clock_hand].read) {
        entries[clock_hand].read = false;
        clock_hand = (clock_hand + 1) % kBucketSize;
        continue;
      }
      entries[clock_hand].signed_tx_hash = signed_tx_hash;
      entries[clock_hand].occupied = true;
      entries[clock_hand].read = false;
      clock_hand = (clock_hand + 1) % kBucketSize;
      return;
    }
  }

  size_t size() const
  {
    size_t occupied = 0;
    for (const Entry &entry : entries) {
      occupied += entry.occupied ? 1 : 0;
    }
    return occupied;
  }

  std::array<Entry, kBucketSize> entries = {};
  size_t clock_hand = 0;
};

} // namespace detail

// Remembers signings that authentication has definitively rejected, so that
// resubmitting the same bytes does not cost another verification.
//
// Keyed on sha256 of the whole [SignedTransaction]. The verdict comes from
// [verify_auth], which consumes signatures, so the key has to cover them: a
// [TransactionHash] covers [Transaction] alone and therefore names every
// signing of a transaction, which would let one submitter's bad copy speak for
// all of them -- including the genuine one.
//
// For public-gateway admission only. Transactions arriving for a nomination
// must bypass it. A verdict here is derived from account keys and weights that
// a later block can change, so entries go stale; on the gateway that costs the
// submitter a resubmission, but on the path that supplies bodies for block
// validation it would keep this node from ever obtaining a body it needs, and
// so from ever validating that block.
//
// Occupancy is bounded structurally at [max_entries]: signings are folded onto
// a fixed number of bucket keys, and each bucket holds a fixed number of
// entries. There is nothing to count and nothing to sweep.
//
// Best-effort in both directions: a miss costs only a reverification, and a
// bucket evicts under the clock policy rather than exactly, so callers must
// treat a hit and a miss alike as advisory.
class FailedSignatureCache {
public:
  // Shard count for the map itself, following the [SynchronizedMap] pattern
  // but at a lower base order than its default: this holds a bounded number of
  // buckets rather than growing with the workload.
  static constexpr int kShardBaseOrder = 4;

  // 8192 buckets of 16 is 128k signings, a few MB once warm.
  explicit FailedSignatureCache(size_t num_buckets = 8192)
    : num_buckets_(num_buckets), buckets_(scalable_shard_size(kShardBaseOrder))
  {}

  bool contains(const SHA256Hash &signed_tx_hash)
  {
    auto [bucket, bucket_lock] = buckets_.find(bucket_key(signed_tx_hash));
    return bucket != nullptr && bucket->contains(signed_tx_hash);
  }

  void insert(const SHA256Hash &signed_tx_hash)
  {
    auto [bucket, bucket_lock] = buckets_[bucket_key(signed_tx_hash)];
    bucket.insert(signed_tx_hash);
  }

  size_t max_entries() const
  {
    return num_buckets_ * detail::FailedSignatureBucket::kBucketSize;
  }

  // Retained signings. Walks every bucket, so this is for diagnostics rather
  // than the admission path.
  size_t size()
  {
    size_t total = 0;
    buckets_.for_each(
        [&total](const std::pair<const uint64_t, detail::FailedSignatureBucket>
                     &bucket) { total += bucket.second.size(); });
    return total;
  }

private:
  uint64_t bucket_key(const SHA256Hash &signed_tx_hash) const
  {
    // Keyed, and random per process. Being a SHA-256 makes this uniform but
    // not unpredictable -- whoever submits the transaction chooses the bytes
    // it covers -- so folding it with an index they could compute offline
    // would let them grind signings into one bucket and push out whatever
    // else it held.
    return SipHash24Hasher{}(signed_tx_hash) % num_buckets_;
  }

  size_t num_buckets_;
  SynchronizedMap<uint64_t, detail::FailedSignatureBucket> buckets_;
};

} // namespace hogd
