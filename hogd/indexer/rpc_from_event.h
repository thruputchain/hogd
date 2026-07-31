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
#include <utility>

namespace hogd {

// Strict production handler for replay and live committed-block delivery.
class IndexerEventRpc {
public:
  using rpc_service = IndexerEvent1;

  IndexerEventRpc(IndexDB &db, RecentTxFailureCache &failure_cache)
    : db_(db), failure_cache_(failure_cache)
  {}

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
  indexer_event_apply_committed_block(IndexerOutboxOrigin origin,
                                      IndexerCommittedBlock block)
  {
    IndexerBlockApplyResult result =
        co_await db_.apply_committed_block(std::move(origin), std::move(block));
    if (result.outcome() == IndexerBlockApplyOutcome::IndexerBlockApplied ||
        result.outcome() == IndexerBlockApplyOutcome::IndexerBlockDuplicate) {
      failure_cache_.prune(result.progress().contiguous_height);
      co_await Health::instance().set_current_block_height(
          result.progress().contiguous_height);
    }
    co_return result;
  }

  x23::Async<void>
  indexer_event_report_tx_failure(const SignedTransactionHash &tx_hash,
                                  const TxFailureDiagnostic &diag)
  {
    failure_cache_.report(tx_hash, diag, db_.latest_block());
    co_return;
  }

private:
  IndexDB &db_;
  RecentTxFailureCache &failure_cache_;
};
static_assert(is_IndexerEvent1<IndexerEventRpc *, x23::Async>);

} // namespace hogd
