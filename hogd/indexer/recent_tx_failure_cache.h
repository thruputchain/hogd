#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "db_conflict.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "transaction.hh"

namespace hogd {

// Short-lived, best-effort cache of recently observed nondeterministic
// transaction failures: tx hash -> TxFailureDiagnostic (attempt height plus
// the first conflicting op).  Fed by the event server's failure-report relay
// (indexer_event_report_tx_failure); consulted by get_transaction_info when
// a tx has no committed record.
//
// Expiry is block-height based, driven by the indexer's contiguous indexed
// watermark: an entry whose latest attempt height is H is dropped once
// prune(watermark) sees watermark >= H + expiry_delta.  Wall-clock plays no
// part, so behavior is deterministic under test.  Modeled on the mempool's
// failed_authentication_txs_ (rpc_from_coord.h).
//
// Memory is strictly bounded: at most [max_entries] fixed-size entries
// (reports for new hashes are dropped when full -- best-effort), and at most
// one expiry row per (hash, attempt height) pair in the live window.
class RecentTxFailureCache {
public:
  static constexpr BlockHeight kDefaultExpiryDelta = 5;
  static constexpr size_t kDefaultMaxEntries = 1 << 16;

  explicit RecentTxFailureCache(BlockHeight expiry_delta = kDefaultExpiryDelta,
                                size_t max_entries = kDefaultMaxEntries)
    : expiry_delta_(expiry_delta), max_entries_(max_entries)
  {}

  // Best-effort insert.  The latest attempt (highest attempt_height) wins
  // for a given hash; a report for a new hash is dropped when the cache is
  // full.
  void report(const SignedTransactionHash &tx_hash,
              const TxFailureDiagnostic &diag)
  {
    const std::lock_guard<std::mutex> guard(mtx_);
    auto it = entries_.find(tx_hash);
    if (it != entries_.end()) {
      if (diag.attempt_height < it->second.attempt_height) {
        // Stale report: keep the newer entry untouched.
        return;
      }
      const bool extends_expiry =
          diag.attempt_height > it->second.attempt_height;
      it->second = diag;
      if (!extends_expiry) {
        // Same height: the existing expiry row already references this hash,
        // so refresh the payload without growing the expiry index.
        return;
      }
    }
    else {
      if (entries_.size() >= max_entries_) {
        return;
      }
      entries_.emplace(tx_hash, diag);
    }
    expiry_index_[diag.attempt_height + expiry_delta_].push_back(tx_hash);
  }

  std::optional<TxFailureDiagnostic>
  lookup(const SignedTransactionHash &tx_hash) const
  {
    const std::lock_guard<std::mutex> guard(mtx_);
    auto it = entries_.find(tx_hash);
    if (it == entries_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  // Drops every entry whose latest attempt height is at least [expiry_delta]
  // blocks behind [watermark].  Called by the ingest path as the indexed
  // watermark advances.
  void prune(BlockHeight watermark)
  {
    const std::lock_guard<std::mutex> guard(mtx_);
    while (!expiry_index_.empty() &&
           expiry_index_.begin()->first <= watermark) {
      for (const SignedTransactionHash &tx_hash :
           expiry_index_.begin()->second) {
        auto it = entries_.find(tx_hash);
        // Only erase if this expiry row covers the entry's current attempt;
        // a re-report at a later height owns a later row.
        if (it != entries_.end() &&
            it->second.attempt_height + expiry_delta_ <= watermark) {
          entries_.erase(it);
        }
      }
      expiry_index_.erase(expiry_index_.begin());
    }
  }

  size_t size() const
  {
    const std::lock_guard<std::mutex> guard(mtx_);
    return entries_.size();
  }

private:
  const BlockHeight expiry_delta_;
  const size_t max_entries_;

  mutable std::mutex mtx_;
  std::unordered_map<SignedTransactionHash, TxFailureDiagnostic,
                     ByteArrayHasher>
      entries_;
  // expiry height -> hashes whose entry (may) die at that height.
  std::map<BlockHeight, std::vector<SignedTransactionHash>> expiry_index_;
};

} // namespace hogd
