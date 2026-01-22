#pragma once

#include "indexer.hh"
#include "x23/async.h"

namespace hogd {

class IndexerEventRpc {
public:
  using rpc_service = IndexerEvent1;

  static x23::Async<void>
  indexer_event_process_events(BlockHeight block_height,
                               TxEventLogBatch tx_events)
  {
    co_return;
  }

  static bool event_server_connected() { return false; }
};
static_assert(is_IndexerEvent1<IndexerEventRpc *, x23::Async>);

} // namespace hogd
