#pragma once

#include <atomic>
#include <cstddef>

#include "hogd/container/synchronized_map.h"
#include "hogd/crypto/crypto.h"
#include "types.hh"

namespace hogd {

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
// Best-effort in both directions: a miss costs only a reverification, and the
// cache is dropped wholesale once full rather than aged entry by entry, so
// callers must treat a hit and a miss alike as advisory.
class FailedSignatureCache {
public:
  // Entries past this are declined, bounding what someone submitting many
  // distinct invalid signings can occupy.
  static constexpr size_t kMaxEntries = 100'000;

  bool contains(const SHA256Hash &signed_tx_hash)
  {
    auto [entry, entry_lock] = entries_.find(signed_tx_hash);
    return entry != nullptr;
  }

  void insert(const SHA256Hash &signed_tx_hash)
  {
    if (size_.load(std::memory_order_relaxed) >= kMaxEntries) {
      return;
    }
    auto [entry, entry_lock] = entries_.insert(signed_tx_hash, 0);
    if (entry != nullptr) {
      size_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // Drops every entry once the cache is full. Cheap to call often; once per
  // committed block is plenty.
  void reclaim_if_full()
  {
    if (size_.load(std::memory_order_relaxed) < kMaxEntries) {
      return;
    }
    entries_.clear();
    size_.store(0, std::memory_order_relaxed);
  }

  size_t size() const { return size_.load(std::memory_order_relaxed); }

private:
  SynchronizedMap<SHA256Hash, char, SipHash24Hasher> entries_;
  std::atomic<size_t> size_ = 0;
};

} // namespace hogd
