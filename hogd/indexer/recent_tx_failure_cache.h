#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "db_conflict.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/container/synchronized_map.h"
#include "transaction.hh"

namespace hogd {

// Short-lived, best-effort cache of recently observed nondeterministic
// transaction failures: tx hash -> TxFailureDiagnostic (attempt height plus
// the first conflicting op).  Fed by the event server's failure-report relay
// (indexer_event_report_tx_failure); consulted by get_transaction_info when a
// tx has no committed record.
//
// Storage is a SynchronizedMap -- a sharded, per-bucket-locked hash table --
// so concurrent reports and lookups from RPC threads contend only when they
// hash to the same shard, matching the structure the mempool uses for its own
// tx-hash sets (failed_authentication_txs_).
//
// Expiry is block-height based, driven by the indexer's contiguous indexed
// watermark: an entry whose latest attempt height is H is dropped once
// prune(watermark) sees watermark >= H + expiry_delta.  Wall-clock plays no
// part, so behavior is deterministic under test.
//
// Memory is bounded by block-height expiry plus a soft cap of [max_entries]
// fixed-size entries: a report for a new hash is dropped once the cap is
// reached (best-effort).
class RecentTxFailureCache {
public:
  static constexpr BlockHeight kDefaultExpiryDelta = 5;
  static constexpr size_t kDefaultMaxEntries = 1 << 16;

  explicit RecentTxFailureCache(BlockHeight expiry_delta = kDefaultExpiryDelta,
                                size_t max_entries = kDefaultMaxEntries)
    : expiry_delta_(expiry_delta), max_entries_(max_entries)
  {}

  // Best-effort insert.  The latest attempt (highest attempt_height) wins for
  // a given hash; a report for a new hash is dropped when the soft cap is hit.
  void report(const SignedTransactionHash &tx_hash,
              const TxFailureDiagnostic &diag)
  {
    auto [existing, lk] = entries_.find(tx_hash);
    if (existing != nullptr) {
      // A stale (lower-height) report never downgrades the entry; a same-or-
      // higher attempt refreshes the payload -- and extends its lifetime,
      // since prune() keys off the stored attempt_height.
      if (diag.attempt_height >= existing->attempt_height) {
        *existing = diag;
      }
      return;
    }
    if (approx_size_.load(std::memory_order_relaxed) >= max_entries_) {
      return;
    }
    entries_.insert(tx_hash, TxFailureDiagnostic(diag), lk);
    approx_size_.fetch_add(1, std::memory_order_relaxed);
  }

  std::optional<TxFailureDiagnostic>
  lookup(const SignedTransactionHash &tx_hash) const
  {
    auto [entry, lk] = entries_.find(tx_hash);
    if (entry == nullptr) {
      return std::nullopt;
    }
    return *entry;
  }

  // Drops every entry whose latest attempt height is at least [expiry_delta]
  // blocks behind [watermark].  Called by the ingest path as the indexed
  // watermark advances.
  void prune(BlockHeight watermark)
  {
    std::vector<SignedTransactionHash> expired;
    entries_.for_each([&](const std::pair<const SignedTransactionHash,
                                          TxFailureDiagnostic> &kv) {
      if (kv.second.attempt_height + expiry_delta_ <= watermark) {
        expired.push_back(kv.first);
      }
    });
    for (const SignedTransactionHash &tx_hash : expired) {
      auto [entry, lk] = entries_.find(tx_hash);
      // Re-check under the bucket lock: a concurrent re-report at a higher
      // attempt height may have refreshed the entry since we scanned it.
      if (entry != nullptr &&
          entry->attempt_height + expiry_delta_ <= watermark) {
        entries_.remove(tx_hash, lk);
        approx_size_.fetch_sub(1, std::memory_order_relaxed);
      }
    }
  }

  size_t size() const { return approx_size_.load(std::memory_order_relaxed); }

private:
  const BlockHeight expiry_delta_;
  const size_t max_entries_;

  // Sharded by tx hash: report()/lookup() lock only the target bucket.
  mutable SynchronizedMap<SignedTransactionHash, TxFailureDiagnostic,
                          ByteArrayHasher>
      entries_;
  // Approximate live-entry count backing size() and the soft cap.  Maintained
  // alongside entries_ under the same per-bucket locks; exact under
  // single-threaded use, approximate under concurrency (acceptable for a
  // best-effort cache).
  std::atomic<size_t> approx_size_ = 0;
};

} // namespace hogd
