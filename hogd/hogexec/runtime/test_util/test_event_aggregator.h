#pragma once

#include <map>
#include <mutex>
#include <optional>

#include "event.hh"
#include "event_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/thread_checked_mutex.h"
#include "hogd/base/types.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
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
  EventBlockResult close_block(BlockID block_id, BlockHeight block_height);
  x23::Async<RPCError> emit_event(BlockID block_id, TransactionID tx_id,
                                  TxEventLog event_log);

  // Executor handle tracking (mirrors real event server behavior).
  void executor_open_block(BlockID block_id);
  void executor_close_block(BlockID block_id);

  // Get all events for a specific block and transaction
  std::optional<TxEventLog> get_events(BlockID block_id, TransactionID tx_id);

  // Get events by transaction hash (linear scan over the block's transactions)
  std::optional<TxEventLog> get_events_by_hash(BlockID block_id,
                                               SHA256Hash const &tx_hash);

  // Blocking control: block emit_event calls until unblock_emit_events is
  // called
  void block_emit_events() { should_block_futex_.store(true); }

  // Unblock emit_event calls (wakes up all waiting calls)
  void unblock_emit_events(x23::ThreadPool *pool = &default_threadpool())
  {
    should_block_futex_.store(false);
    should_block_futex_.broadcast(pool);
  }

private:
  ThreadCheckedMutex<std::mutex> mtx_;
  x23::CoroFutex<bool> should_block_futex_{false};
  // Track opened/closed blocks (from sched side)
  std::map<BlockID, int32_t> block_refcounts_;
  // Track executor handles per block (from exec side)
  std::map<BlockID, int32_t> executor_handle_refcounts_;

  RPCError do_emit_event(BlockID block_id, TransactionID tx_id,
                         TxEventLog event_log);

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

  x23::Async<RPCError> eventexecutor_open_block(BlockID block_id)
  {
    event_aggregator_.executor_open_block(block_id);
    co_return RPCError::kRPCNone;
  }

  x23::Async<RPCError> eventexecutor_close_block(BlockID block_id)
  {
    event_aggregator_.executor_close_block(block_id);
    co_return RPCError::kRPCNone;
  }

  x23::Async<RPCError> eventexecutor_emit_event(BlockID block_id,
                                                TransactionID tx_id,
                                                TxEventLog const &event_log)
  {
    return event_aggregator_.emit_event(block_id, tx_id, event_log);
  }

private:
  TestEventAggregator &event_aggregator_;
};
static_assert(is_EventExecutor1<TestEventAggregatorExecRpc *, x23::MaybeAsync>);

class TestEventAggregatorExecServer
  : public RpcServer<TestEventAggregatorExecRpc> {
public:
  TestEventAggregatorExecServer(TestEventAggregator &event_aggregator)
    : RpcServer(event_aggregator)
  {}
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

  EventBlockResult eventsched_close_block(BlockID block_id,
                                          BlockHeight block_height)
  {
    return event_aggregator_.close_block(block_id, block_height);
  }

  RPCError eventsched_abandon_block(BlockID block_id)
  {
    return event_aggregator_.close_block(block_id, /*block_height=*/0).error();
  }

private:
  TestEventAggregator &event_aggregator_;
};
static_assert(is_EventSched1<TestEventAggregatorSchedRpc *, x23::MaybeAsync>);

class TestEventAggregatorSchedServer
  : public RpcServer<TestEventAggregatorSchedRpc> {
public:
  TestEventAggregatorSchedServer(TestEventAggregator &event_aggregator)
    : RpcServer(event_aggregator)
  {}
};

} // namespace hogd
