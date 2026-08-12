#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "db_conflict.hh"
#include "hogd/base/log.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/crypto/crypto.h"
#include "transaction.hh"
#include "types.hh"

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
  // Reports whose attempt height is further than this ahead of the reporting
  // watermark are rejected: an absurd height (buggy peer, corruption) would
  // otherwise never satisfy the expiry test and occupy a cap slot forever.
  // Generous on purpose -- a fresh or lagging indexer legitimately receives
  // real-time failure reports far ahead of its own watermark, and any
  // accepted entry still expires within slack + expiry_delta blocks of
  // ingest progress, so the cache always self-heals.
  static constexpr BlockHeight kMaxAttemptHeightSlack = BlockHeight{1} << 20;
  // This cache is tiny (only nondeterministic failures, expiring in a few
  // blocks) and low-QPS, and prune() scans every shard each block -- so a
  // modest fixed shard count beats SynchronizedMap's default (hundreds to
  // thousands of shards, sized to core count), which would tax every prune
  // with no concurrency benefit here.
  static constexpr size_t kShardCount = 64;

  explicit RecentTxFailureCache(BlockHeight expiry_delta = kDefaultExpiryDelta,
                                size_t max_entries = kDefaultMaxEntries)
    : expiry_delta_(expiry_delta), max_entries_(max_entries),
      entries_(kShardCount)
  {}

  // Best-effort insert.  The latest attempt (highest attempt_height) wins for
  // a given hash; a report for a new hash is dropped when the soft cap is
  // hit.  [watermark] is the caller's current indexed height, used only to
  // reject implausible attempt heights (see kMaxAttemptHeightSlack).
  void report(const SignedTransactionHash &tx_hash,
              const TxFailureDiagnostic &diag, BlockHeight watermark)
  {
    // Malformed or absurd heights are wire input from a peer; drop them
    // rather than let an unprunable entry poison the cap.
    if (diag.attempt_height < 0 ||
        diag.attempt_height > watermark + kMaxAttemptHeightSlack) {
      warn("dropping tx-failure report with implausible attempt_height {} "
           "(watermark {})",
           diag.attempt_height, watermark);
      return;
    }
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
    // Subtract-form comparison (attempt <= watermark - delta) so a stored
    // height near INT64_MAX can never overflow the arithmetic; equivalent
    // for all in-range values.
    std::vector<SignedTransactionHash> expired;
    entries_.for_each([&](const std::pair<const SignedTransactionHash,
                                          TxFailureDiagnostic> &kv) {
      if (kv.second.attempt_height <= watermark - expiry_delta_) {
        expired.push_back(kv.first);
      }
    });
    for (const SignedTransactionHash &tx_hash : expired) {
      auto [entry, lk] = entries_.find(tx_hash);
      // Re-check under the bucket lock: a concurrent re-report at a higher
      // attempt height may have refreshed the entry since we scanned it.
      if (entry != nullptr &&
          entry->attempt_height <= watermark - expiry_delta_) {
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
                          SipHash24Hasher>
      entries_;
  // Approximate live-entry count backing size() and the soft cap.  Maintained
  // alongside entries_ under the same per-bucket locks; exact under
  // single-threaded use, approximate under concurrency (acceptable for a
  // best-effort cache).
  std::atomic<size_t> approx_size_ = 0;
};

} // namespace hogd
