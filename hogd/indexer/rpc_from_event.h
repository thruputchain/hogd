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

  // [tx_events] and [tx_bodies] are index-aligned: [tx_bodies][i] is the body
  // of the transaction whose event log is [tx_events][i].
  //
  // [tx_bodies] are consumed into the tx_index value (serving by-hash and
  // transaction-page lookups) and used to key ROOT_TX rows in the address
  // index (serving get_transactions_by_address).
  //
  // TODO(gramseyer): move body storage out of the tx_index value into a
  // dedicated flat-file body store, keyed by [tx_events][i].proposed_tx_hash.
  x23::Async<void>
  indexer_event_process_events(BlockHeight block_height,
                               const TxEventLogBatch &tx_events,
                               const ProposedTransactionBatch &tx_bodies)
  {
    // Careful! [tx_events] and [tx_bodies] are both references. If we change
    // this function to use [co_await] *before* they are accessed, be sure to
    // copy those parameters above rather than keep them as references.
    CHECK(tx_events.size() == tx_bodies.size());
    CHECK(db_.consume_events(block_height, tx_events, tx_bodies).has_value());
    // Expire failure diagnostics as the indexed watermark advances.
    failure_cache_.prune(db_.latest_block());
    co_await Health::instance().set_current_block_height(block_height);
    co_return;
  }

  // Best-effort diagnostic (relayed by the event server) for a tx that
  // failed nondeterministically at proposal time. Cached briefly so
  // get_transaction_info can explain the failure.
  x23::Async<void>
  indexer_event_report_tx_failure(const SignedTransactionHash &tx_hash,
                                  const TxFailureDiagnostic &diag)
  {
    // Careful! Both parameters are references; copy them before any
    // co_await is introduced ahead of their use.
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
