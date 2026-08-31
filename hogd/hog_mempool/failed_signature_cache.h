#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_mempool/recent_key_changes.h"
#include "types.hh"

namespace hogd {

namespace detail {

// A fixed-size set of signings sharing one bucket, evicted by clock: a lookup
// sets the read bit, an insert advances the hand past set bits, clearing them,
// until it finds a slot. Signings that keep being resubmitted survive.
//
// Not synchronized; callers hold the [SynchronizedMap] bucket lock.
struct FailedSignatureBucket {
  constexpr static size_t kBucketSize = 16;

  struct Entry {
    SHA256Hash signed_tx_hash = {};
    BlockHeight verified_at_height = 0;
    bool occupied = false;
    bool read = false;
  };

  std::optional<BlockHeight> verified_at(const SHA256Hash &signed_tx_hash)
  {
    for (Entry &entry : entries) {
      if (entry.occupied && entry.signed_tx_hash == signed_tx_hash) {
        entry.read = true;
        return entry.verified_at_height;
      }
    }
    return std::nullopt;
  }

  void insert(const SHA256Hash &signed_tx_hash, BlockHeight verified_at_height)
  {
    for (Entry &entry : entries) {
      if (entry.occupied && entry.signed_tx_hash == signed_tx_hash) {
        entry.read = true;
        entry.verified_at_height = verified_at_height;
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
      entries[clock_hand].verified_at_height = verified_at_height;
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

// Signings authentication has rejected, so resubmitting the same bytes does
// not cost another verification.
//
// Keyed on sha256 of the whole [SignedTransaction]: the verdict comes from
// [verify_auth], which consumes signatures, so the key must cover them.
// [TransactionHash] excludes them, and would let one bad copy speak for every
// signing of that transaction, the genuine one included.
//
// Verdicts carry the height they were taken at and are honoured only while
// [RecentKeyChanges] says the account's keys have not moved since. Nothing is
// dropped on a key change; entries just stop counting.
//
// Gateway admission only. Nomination traffic must bypass it: a stale entry on
// the path that supplies bodies for block validation would keep this node from
// obtaining one it needs.
//
// Bounded structurally at [max_entries]. Best-effort: a miss costs only a
// reverification and eviction is approximate, so treat hits and misses alike
// as advisory.
class FailedSignatureCache {
public:
  // 8192 buckets of 16 is 128k signings, a few MB once warm.
  explicit FailedSignatureCache(size_t num_buckets = 8192)
    : num_buckets_(num_buckets)
  {}

  bool contains(const ContractAddress &address,
                const SHA256Hash &signed_tx_hash)
  {
    std::optional<BlockHeight> verified_at;
    {
      auto [bucket, bucket_lock] = buckets_.find(bucket_key(signed_tx_hash));
      if (bucket == nullptr) {
        return false;
      }
      verified_at = bucket->verified_at(signed_tx_hash);
    }
    return verified_at.has_value() &&
           key_changes_.still_current(address, *verified_at);
  }

  void insert(const SHA256Hash &signed_tx_hash, BlockHeight verified_at_height)
  {
    auto [bucket, bucket_lock] = buckets_[bucket_key(signed_tx_hash)];
    bucket.insert(signed_tx_hash, verified_at_height);
  }

  void note_key_change(const ContractAddress &address, BlockHeight height)
  {
    key_changes_.note(address, height);
  }

  size_t max_entries() const
  {
    return num_buckets_ * detail::FailedSignatureBucket::kBucketSize;
  }

  // Locks every bucket in turn; for diagnostics, not the admission path.
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
    // Keyed per process, so a signing's bucket cannot be worked out offline
    // and ground into. Being a SHA-256 makes it uniform, not unpredictable:
    // the submitter chooses the bytes it covers.
    return SipHash24Hasher{}(signed_tx_hash) % num_buckets_;
  }

  size_t num_buckets_;
  SynchronizedMap<uint64_t, detail::FailedSignatureBucket> buckets_;
  RecentKeyChanges key_changes_;
};

} // namespace hogd
