#pragma once

#include <map>
#include <mutex>
#include <optional>

#include "event.hh"
#include "event_rpc.hh"
#include "hogd/base/types.h"
#include "rpc_types.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace hogd {

// Test RPC service that implements ExecutorEvent1 and stores all inputs in std::map
// for later inspection in tests
class TestEventAggregator {
public:
  using rpc_service = ExecutorEvent1;

  TestEventAggregator() = default;

  // Disable copy and move
  TestEventAggregator(TestEventAggregator const &) = delete;
  TestEventAggregator &operator=(TestEventAggregator const &) = delete;
  TestEventAggregator(TestEventAggregator &&) = delete;
  TestEventAggregator &operator=(TestEventAggregator &&) = delete;

  uint16_t start_server(uint16_t port_number);

  // RPC service methods
  static void executorevent_null() { }

  RPCError executorevent_open_block(BlockID block_id);
  RPCError executorevent_close_block(BlockID block_id);
  RPCError executorevent_emit_event(BlockID block_id,
                                    TransactionID tx_id,
                                    TxEventLog const &event_log);

  // Get all events for a specific block and transaction
  std::optional<TxEventLog> get_events(BlockID block_id, TransactionID tx_id);

private:
  x23::ThreadPool pool_ = x23::ThreadPool(1);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);

  std::mutex mtx_;
  // Track opened/closed blocks
  std::map<BlockID, int32_t> block_refcounts_;

  bool is_block_open(BlockID block_id) const {
    return block_refcounts_.contains(block_id) && block_refcounts_.at(block_id) > 0;
  }
  
  // Store events: block_id -> tx_id -> vector of events
  std::map<BlockID, std::map<TransactionID, TxEventLog>> events_;
};

static_assert(is_ExecutorEvent1<TestEventAggregator *, x23::MaybeAsync>);

} // namespace hogd
