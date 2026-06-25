#pragma once

#include "hogd/base/log.h"
#include "hogd/indexer/db.h"
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

  IndexerEventRpc(IndexDB &db) : db_(db) {}

  x23::Async<void> indexer_event_intro()
  {
    event_server_connected_.store(true, std::memory_order_release);
    co_return;
  }

  // [tx_events] and [tx_bodies] are index-aligned: [tx_bodies][i] is the body
  // of the transaction whose event log is [tx_events][i].
  //
  // TODO(gramseyer): persist [tx_bodies] to the flat-file body store, keyed by
  // [tx_events][i].proposed_tx_hash.  For now only the event log is processed;
  // the bodies are accepted and dropped until the store lands in a follow-up.
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
    co_await Health::instance().set_current_block_height(block_height);
    co_return;
  }

  bool event_server_connected() const
  {
    return event_server_connected_.load(std::memory_order_acquire);
  }

private:
  IndexDB &db_;
  std::atomic<bool> event_server_connected_ = false;
};
static_assert(is_IndexerEvent1<IndexerEventRpc *, x23::Async>);

} // namespace hogd
