#pragma once

#include "db_conflict.hh"
#include "hogd/base/log.h"
#include "hogd/indexer/db.h"
#include "hogd/indexer/recent_tx_failure_cache.h"
#include "hogd/metrics/health.h"
#include "indexer.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include <atomic>

namespace hogd {

// Strict production handler for replay and live committed-block delivery.
class IndexerEventRpcV2 {
public:
  using rpc_service = IndexerEvent2;

  IndexerEventRpcV2(IndexDB &db, RecentTxFailureCache &failure_cache)
    : db_(db), failure_cache_(failure_cache)
  {}

  x23::Async<void> indexer_event_intro()
  {
    event_server_connected_.store(true, std::memory_order_release);
    co_return;
  }

  x23::Async<IndexerProgress> indexer_event_get_progress()
  {
    co_return db_.durable_progress();
  }

  x23::Async<IndexerEpochResult>
  indexer_event_initialize_epoch(const IndexerOutboxOrigin &origin)
  {
    co_return db_.initialize_epoch(origin);
  }

  x23::Async<IndexerBlockApplyResult>
  indexer_event_apply_committed_block(const IndexerOutboxOrigin &origin,
                                      const IndexerCommittedBlock &block)
  {
    IndexerBlockApplyResult result = db_.apply_committed_block(origin, block);
    if (result.outcome() == IndexerBlockApplyOutcome::IndexerBlockApplied ||
        result.outcome() == IndexerBlockApplyOutcome::IndexerBlockDuplicate) {
      failure_cache_.prune(result.progress().contiguous_height);
      co_await Health::instance().set_current_block_height(
          result.progress().contiguous_height);
    }
    co_return result;
  }

  x23::Async<void>
  indexer_event_report_tx_failure_v2(const SignedTransactionHash &tx_hash,
                                     const TxFailureDiagnostic &diag)
  {
    failure_cache_.report(tx_hash, diag, db_.latest_block());
    co_return;
  }

  bool event_server_connected() const
  {
    return event_server_connected_.load(std::memory_order_acquire);
  }

private:
  IndexDB &db_;
  RecentTxFailureCache &failure_cache_;
  std::atomic<bool> event_server_connected_ = false;
};
static_assert(is_IndexerEvent2<IndexerEventRpcV2 *, x23::Async>);

// Compatibility-only V1 handler retained for legacy storage tests. Production
// event, indexer, and devnode startup do not serve or dial this RPC version.
class IndexerEventRpc {
public:
  using rpc_service = IndexerEvent1;

  IndexerEventRpc(IndexDB &db, RecentTxFailureCache &failure_cache)
    : db_(db), failure_cache_(failure_cache)
  {}

  x23::Async<void> indexer_event_intro()
  {
    event_server_connected_.store(true, std::memory_order_release);
    co_return;
  }

  x23::Async<void>
  indexer_event_process_events(BlockHeight block_height,
                               const TxEventLogBatch &tx_events,
                               const ProposedTransactionBatch &tx_bodies)
  {
    // [tx_events] and [tx_bodies] are index-aligned: [tx_bodies][i] is the body
    // of the transaction whose event log is [tx_events][i]. The bodies are
    // stored for transaction lookups and root-address history.
    //
    // Careful: both batches are references. Copy them before introducing any
    // co_await ahead of their use.
    CHECK(tx_events.size() == tx_bodies.size());
    CHECK(db_.consume_events(block_height, tx_events, tx_bodies).has_value());
    failure_cache_.prune(db_.latest_block());
    co_await Health::instance().set_current_block_height(block_height);
    co_return;
  }

  x23::Async<void>
  indexer_event_report_tx_failure(const SignedTransactionHash &tx_hash,
                                  const TxFailureDiagnostic &diag)
  {
    // Both parameters are references; copy them before introducing any
    // co_await ahead of their use.
    failure_cache_.report(tx_hash, diag, db_.latest_block());
    co_return;
  }

  bool event_server_connected() const
  {
    return event_server_connected_.load(std::memory_order_acquire);
  }

private:
  IndexDB &db_;
  RecentTxFailureCache &failure_cache_;
  std::atomic<bool> event_server_connected_ = false;
};
static_assert(is_IndexerEvent1<IndexerEventRpc *, x23::Async>);

} // namespace hogd
