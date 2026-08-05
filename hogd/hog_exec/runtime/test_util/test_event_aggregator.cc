#include "hogd/hog_exec/runtime/test_util/test_event_aggregator.h"

#include <optional>
#include <utility>
#include <vector>

#include "consensus.hh"
#include "db_conflict.hh"
#include "event.hh"
#include "event_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "proposal.hh"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"

namespace hogd {

x23::Async<RPCError>
TestEventAggregator::open_block(BlockID block_id)
{
  info("Opening block {}", block_id);
  x23::CoroGuard guard;
  co_await guard(lock_);
  block_states_[block_id] = BlockState::kActive;
  co_return RPCError::kRPCNone;
}

x23::Async<EventBlockResult>
TestEventAggregator::close_block(BlockID block_id, BlockHeight block_height)
{
  // Wait for all executor handles to close before proceeding,
  // matching the real event server's wait_for_executors_closed behavior.
  while (true) {
    int32_t executor_refcount = 0;
    {
      x23::CoroGuard guard;
      co_await guard(lock_);
      executor_refcount = executor_handle_refcounts_[block_id];
    }
    if (executor_refcount <= 0) {
      break;
    }
    co_await default_threadpool().bg();
  }

  x23::CoroGuard guard;
  co_await guard(lock_);
  auto it = block_states_.find(block_id);
  if (it == block_states_.end() || it->second != BlockState::kActive) {
    error("Block {} is not open", block_id);
    co_return EventBlockResult(RPCError::kRPCBadBlockID);
  }

  it->second = BlockState::kClosed;
  SHA256Hash root_hash{}; // TODO(geoff): Implement
  EventBlockResult result(RPCError::kRPCNone);
  result.root_hash() = root_hash;
  info("Closed block {}, height = {}", block_id, block_height);
  co_return result;
}

x23::Async<EventBlockResult>
TestEventAggregator::close_block(BlockID block_id, BlockHeight block_height,
                                 ProposalTxs proposal_txs)
{
  {
    x23::CoroGuard guard;
    co_await guard(lock_);
    closed_proposal_txs_[block_id] = std::move(proposal_txs);
  }
  co_return co_await close_block(block_id, block_height);
}

x23::Async<RPCError>
TestEventAggregator::commit_block(BlockID block_id,
                                  ConsensusValue consensus_value)
{
  x23::CoroGuard guard;
  co_await guard(lock_);
  committed_consensus_values_[block_id] = std::move(consensus_value);
  co_return RPCError::kRPCNone;
}

x23::Async<std::optional<ProposalTxs>>
TestEventAggregator::get_closed_proposal_txs(BlockID block_id)
{
  x23::CoroGuard guard;
  co_await guard(lock_);
  auto it = closed_proposal_txs_.find(block_id);
  co_return it == closed_proposal_txs_.end()
      ? std::nullopt
      : std::optional<ProposalTxs>(it->second);
}

x23::Async<std::optional<ConsensusValue>>
TestEventAggregator::get_committed_consensus_value(BlockID block_id)
{
  x23::CoroGuard guard;
  co_await guard(lock_);
  auto it = committed_consensus_values_.find(block_id);
  co_return it == committed_consensus_values_.end()
      ? std::nullopt
      : std::optional<ConsensusValue>(it->second);
}

x23::Async<RPCError>
TestEventAggregator::abandon_block(BlockID block_id)
{
  x23::CoroGuard guard;
  co_await guard(lock_);
  auto it = block_states_.find(block_id);
  if (it == block_states_.end()) {
    error("Block {} is not open", block_id);
    co_return RPCError::kRPCBadBlockID;
  }
  // Abandon accepts both active and already-closed blocks. Erasing the entry
  // (like the real event server tearing down the block context) immediately
  // causes concurrent emit_event calls to short-circuit, since is_block_open
  // becomes false under lock_.
  block_states_.erase(it);
  info("Abandoned block {}", block_id);
  co_return RPCError::kRPCNone;
}

x23::Async<void>
TestEventAggregator::executor_open_block(BlockID block_id)
{
  x23::CoroGuard guard;
  co_await guard(lock_);
  executor_handle_refcounts_[block_id]++;
  co_return;
}

x23::Async<void>
TestEventAggregator::executor_close_block(BlockID block_id)
{
  x23::CoroGuard guard;
  co_await guard(lock_);
  executor_handle_refcounts_[block_id]--;
  co_return;
}

x23::Async<RPCError>
TestEventAggregator::do_emit_event(BlockID block_id, TransactionID tx_id,
                                   TxEventLog event_log)
{
  x23::CoroGuard guard;
  co_await guard(lock_);
  if (!is_block_open(block_id)) {
    error("Block {} is not open", block_id);
    co_return RPCError::kRPCBadBlockID;
  }
  if (events_[block_id].contains(tx_id)) {
    error("Unrecoverable: Tx {} already exists for block {}", tx_id, block_id);
    co_return RPCError::kRPCUnrecoverable;
  }
  events_[block_id][tx_id] = std::move(event_log);
  co_return RPCError::kRPCNone;
}

x23::Async<RPCError>
TestEventAggregator::emit_event(BlockID block_id, TransactionID tx_id,
                                TxEventLog event_log)
{
  co_await should_block_futex_.wait(true);
  co_return co_await do_emit_event(block_id, tx_id, std::move(event_log));
}

x23::Async<std::optional<TxEventLog>>
TestEventAggregator::get_events(BlockID block_id, TransactionID tx_id)
{
  x23::CoroGuard guard;
  co_await guard(lock_);

  auto it = events_.find(block_id);
  if (it == events_.end()) {
    error("Block {} not found", block_id);
    co_return std::nullopt;
  }

  if (!it->second.contains(tx_id)) {
    error("Tx {} not found in block {}", tx_id, block_id);
    co_return std::nullopt;
  }
  co_return std::optional<TxEventLog>(it->second.at(tx_id));
}

x23::Async<std::optional<TxEventLog>>
TestEventAggregator::get_events_by_hash(BlockID block_id,
                                        SHA256Hash const &tx_hash)
{
  x23::CoroGuard guard;
  co_await guard(lock_);

  auto it = events_.find(block_id);
  if (it == events_.end()) {
    error("Block {} not found", block_id);
    co_return std::nullopt;
  }

  for (auto const &[tx_id, event_log] : it->second) {
    if (event_log.proposed_tx_hash == tx_hash) {
      co_return std::optional<TxEventLog>(event_log);
    }
  }
  error("Tx hash not found in block {}", block_id);
  co_return std::nullopt;
}

x23::Async<RPCError>
TestEventAggregator::report_tx_failure(SignedTransactionHash tx_hash,
                                       TxFailureDiagnostic diag)
{
  {
    x23::CoroGuard cg;
    co_await cg(lock_);
    failure_reports_.emplace_back(std::move(tx_hash), diag);
  }
  failure_report_count_.rmw([](int64_t &count) { count++; });
  failure_report_count_.broadcast(&default_threadpool());
  co_return RPCError::kRPCNone;
}

x23::Async<void>
TestEventAggregator::wait_for_failure_reports(int64_t n)
{
  while (true) {
    const int64_t current = failure_report_count_.load();
    if (current >= n) {
      co_return;
    }
    co_await failure_report_count_.wait(current);
  }
}

x23::Async<std::vector<std::pair<SignedTransactionHash, TxFailureDiagnostic>>>
TestEventAggregator::get_failure_reports()
{
  x23::CoroGuard cg;
  co_await cg(lock_);
  auto copy = failure_reports_;
  co_return copy;
}

} // namespace hogd
