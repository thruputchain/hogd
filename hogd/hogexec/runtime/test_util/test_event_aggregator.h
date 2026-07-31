#pragma once

#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "db_conflict.hh"
#include "event.hh"
#include "event_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "rpc_types.hh"
#include "transaction.hh"
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

  x23::Async<RPCError> open_block(BlockID block_id);
  x23::Async<EventBlockResult> close_block(BlockID block_id,
                                           BlockHeight block_height);
  x23::Async<EventBlockResult> close_block(BlockID block_id,
                                           BlockHeight block_height,
                                           ProposalTxs proposal_txs);
  x23::Async<RPCError> commit_block(BlockID block_id,
                                    ConsensusValue consensus_value);
  x23::Async<std::optional<ProposalTxs>>
  get_closed_proposal_txs(BlockID block_id);
  x23::Async<std::optional<ConsensusValue>>
  get_committed_consensus_value(BlockID block_id);
  x23::Async<RPCError> abandon_block(BlockID block_id);
  x23::Async<RPCError> emit_event(BlockID block_id, TransactionID tx_id,
                                  TxEventLog event_log);

  // Executor handle tracking (mirrors real event server behavior).
  x23::Async<void> executor_open_block(BlockID block_id);
  x23::Async<void> executor_close_block(BlockID block_id);

  // Get all events for a specific block and transaction
  x23::Async<std::optional<TxEventLog>> get_events(BlockID block_id,
                                                   TransactionID tx_id);

  // Get events by transaction hash (linear scan over the block's transactions)
  x23::Async<std::optional<TxEventLog>>
  get_events_by_hash(BlockID block_id, SHA256Hash const &tx_hash);

  // Records a forwarded tx-failure diagnostic (executor -> event server).
  x23::Async<RPCError> report_tx_failure(SignedTransactionHash tx_hash,
                                         TxFailureDiagnostic diag);

  // Blocks until at least [n] failure reports have arrived.
  x23::Async<void> wait_for_failure_reports(int64_t n);

  x23::Async<std::vector<std::pair<SignedTransactionHash, TxFailureDiagnostic>>>
  get_failure_reports();

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
  // Per-block lifecycle state (from sched side), mirroring EventBlockContext's
  // active/closed distinction:
  //   kActive -> kClosed   (close_block)
  // open_block (re)inserts the block as kActive; abandon_block erases the entry
  // entirely (from either state). A block with no entry is unknown/abandoned,
  // so concurrent emit_event calls short-circuit (is_block_open is false).
  enum class BlockState : int32_t {
    kActive,
    kClosed,
  };

  x23::CoroLock lock_;
  x23::CoroFutex<bool> should_block_futex_{false};
  x23::CoroFutex<int64_t> failure_report_count_{0};
  std::vector<std::pair<SignedTransactionHash, TxFailureDiagnostic>>
      failure_reports_;
  // Track block lifecycle state (from sched side)
  std::map<BlockID, BlockState> block_states_;
  // Track executor handles per block (from exec side)
  std::map<BlockID, int32_t> executor_handle_refcounts_;
  std::map<BlockID, ProposalTxs> closed_proposal_txs_;
  std::map<BlockID, ConsensusValue> committed_consensus_values_;

  x23::Async<RPCError> do_emit_event(BlockID block_id, TransactionID tx_id,
                                     TxEventLog event_log);

  bool is_block_open(BlockID block_id) const
  {
    auto it = block_states_.find(block_id);
    return it != block_states_.end() && it->second == BlockState::kActive;
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
    co_await event_aggregator_.executor_open_block(block_id);
    co_return RPCError::kRPCNone;
  }

  x23::Async<RPCError> eventexecutor_close_block(BlockID block_id)
  {
    co_await event_aggregator_.executor_close_block(block_id);
    co_return RPCError::kRPCNone;
  }

  x23::Async<RPCError>
  eventexecutor_emit_event(BlockID block_id, TransactionID tx_id,
                           TxEventLog const &event_log,
                           ProposedTransaction const & /*proposed_tx*/)
  {
    return event_aggregator_.emit_event(block_id, tx_id, event_log);
  }

  x23::Async<RPCError>
  eventexecutor_report_tx_failure(SignedTransactionHash tx_hash,
                                  TxFailureDiagnostic diag)
  {
    return event_aggregator_.report_tx_failure(std::move(tx_hash),
                                               std::move(diag));
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

  using rpc_service = EventSched2;

  static void eventsched2_null() {}

  x23::Async<RPCError> eventsched2_open_block(BlockID block_id)
  {
    return event_aggregator_.open_block(block_id);
  }

  x23::Async<EventBlockResult> eventsched2_close_block(BlockID block_id,
                                                       BlockHeight block_height,
                                                       ProposalTxs txs)
  {
    return event_aggregator_.close_block(block_id, block_height,
                                         std::move(txs));
  }

  x23::Async<RPCError> eventsched2_commit_block(BlockID block_id,
                                                ConsensusValue value)
  {
    return event_aggregator_.commit_block(block_id, std::move(value));
  }

  // Keep the by-value parameter required by the generated EventSched2 service.
  static RPCError eventsched2_confirm_committed_block(
      ConsensusValue /*value*/) // NOLINT(performance-unnecessary-value-param)
  {
    return RPCError::kRPCNone;
  }

  x23::Async<RPCError> eventsched2_abandon_block(BlockID block_id)
  {
    return event_aggregator_.abandon_block(block_id);
  }

  x23::Async<std::optional<ProposalTxs>>
  get_closed_proposal_txs(BlockID block_id)
  {
    return event_aggregator_.get_closed_proposal_txs(block_id);
  }

  x23::Async<std::optional<ConsensusValue>>
  get_committed_consensus_value(BlockID block_id)
  {
    return event_aggregator_.get_committed_consensus_value(block_id);
  }

private:
  TestEventAggregator &event_aggregator_;
};
static_assert(is_EventSched2<TestEventAggregatorSchedRpc *, x23::MaybeAsync>);

class TestEventAggregatorSchedServer
  : public RpcServer<TestEventAggregatorSchedRpc> {
public:
  TestEventAggregatorSchedServer(TestEventAggregator &event_aggregator)
    : RpcServer(event_aggregator)
  {}
};

} // namespace hogd
