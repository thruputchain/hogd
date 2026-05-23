#pragma once

#include "hogd/base/log.h"
#include "hogd/health/health.h"
#include "hogd/indexer/db.h"
#include "indexer.hh"
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

  x23::Async<void>
  indexer_event_process_events(BlockHeight block_height,
                               const TxEventLogBatch &tx_events)
  {
    // Careful! If we change this function to use [co_await], but sure to copy
    // the [tx_events] parameter above rather than keep it as a reference.
    CHECK(db_.consume_events(block_height, tx_events).has_value());
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
