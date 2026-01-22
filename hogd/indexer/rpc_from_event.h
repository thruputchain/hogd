#pragma once

#include "indexer.hh"
#include "x23/async.h"

namespace hogd {

class IndexerEventRpc {
public:
  using rpc_service = IndexerEvent1;

  x23::Async<void> indexer_event_intro()
  {
    event_server_connected_.store(true, std::memory_order_release);
    co_return;
  }

  static x23::Async<void>
  indexer_event_process_events(BlockHeight block_height,
                               TxEventLogBatch tx_events)
  {
    co_return;
  }

  bool event_server_connected() const
  {
    return event_server_connected_.load(std::memory_order_acquire);
  }

private:
  std::atomic<bool> event_server_connected_ = false;
};
static_assert(is_IndexerEvent1<IndexerEventRpc *, x23::Async>);

} // namespace hogd
