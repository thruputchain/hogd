#pragma once

#include <map>
#include <mutex>
#include <optional>

#include "event.hh"
#include "event_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "rpc_types.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"

namespace hogd {

// Test RPC service that implements ExecutorEvent1 and stores all inputs in
// std::map for later inspection in tests
class TestEventAggregator {
public:
  TestEventAggregator() = default;

  // Disable copy and move
  TestEventAggregator(TestEventAggregator const &) = delete;
  TestEventAggregator &operator=(TestEventAggregator const &) = delete;
  TestEventAggregator(TestEventAggregator &&) = delete;
  TestEventAggregator &operator=(TestEventAggregator &&) = delete;

  RPCError open_block(BlockID block_id);
  EventBlockResult close_block(BlockID block_id);
  RPCError emit_event(BlockID block_id, TransactionID tx_id,
                      TxEventLog const &event_log);

  // Get all events for a specific block and transaction
  std::optional<TxEventLog> get_events(BlockID block_id, TransactionID tx_id);

  auto &get_coropoll() { return cp_; }

private:
  x23::ThreadPool pool_ = x23::ThreadPool(1);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);

  std::mutex mtx_;
  // Track opened/closed blocks
  std::map<BlockID, int32_t> block_refcounts_;

  bool is_block_open(BlockID block_id) const
  {
    return block_refcounts_.contains(block_id) &&
           block_refcounts_.at(block_id) > 0;
  }

  // Store events: block_id -> tx_id -> vector of events
  std::map<BlockID, std::map<TransactionID, TxEventLog>> events_;
};

struct TestEventAggregatorExecRpc {
  TestEventAggregatorExecRpc(TestEventAggregator &event_aggregator)
    : event_aggregator_(event_aggregator)
  {}

  using rpc_service = EventExecutor1;

  static void eventexecutor_null() {}

  RPCError eventexecutor_emit_event(BlockID block_id, TransactionID tx_id,
                                    TxEventLog const &event_log)
  {
    return event_aggregator_.emit_event(block_id, tx_id, event_log);
  }

  uint16_t start_server(uint16_t port_number)
  {
    auto ret = tcp_rpc_server(event_aggregator_.get_coropoll(), port_number,
                              [this](auto c) { c->server(this); });
    info("Starting event exec (test impl) RPC server on port {}", ret);
    return ret;
  }

private:
  TestEventAggregator &event_aggregator_;
};

struct TestEventAggregatorSchedRpc {
  TestEventAggregatorSchedRpc(TestEventAggregator &event_aggregator)
    : event_aggregator_(event_aggregator)
  {}

  using rpc_service = EventSched1;

  static void eventsched_null() {}

  RPCError eventsched_open_block(BlockID block_id)
  {
    return event_aggregator_.open_block(block_id);
  }

  EventBlockResult eventsched_close_block(BlockID block_id)
  {
    return event_aggregator_.close_block(block_id);
  }

  void eventsched_abandon_block(BlockID block_id)
  {
    event_aggregator_.close_block(block_id);
  }

  uint16_t start_server(uint16_t port_number)
  {
    auto ret = tcp_rpc_server(event_aggregator_.get_coropoll(), port_number,
                              [this](auto c) { c->server(this); });
    info("Starting event sched (test impl) RPC server on port {}", ret);
    return ret;
  }

private:
  TestEventAggregator &event_aggregator_;
};

static_assert(is_EventSched1<TestEventAggregatorSchedRpc *, x23::MaybeAsync>);

static_assert(is_EventExecutor1<TestEventAggregatorExecRpc *, x23::MaybeAsync>);

} // namespace hogd
