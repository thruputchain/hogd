#include "hogd/hogexec/runtime/test_util/test_event_aggregator.h"

#include <mutex>
#include <optional>
#include <utility>

#include "event.hh"
#include "event_rpc.hh"
#include "hogd/base/log.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

RPCError
TestEventAggregator::open_block(BlockID block_id)
{
  info("Opening block {}", block_id);
  std::scoped_lock lock(mtx_);
  block_refcounts_[block_id]++;
  return RPCError::kRPCNone;
}

EventBlockResult
TestEventAggregator::close_block(BlockID block_id, BlockHeight block_height)
{
  // Wait for all executor handles to close before proceeding,
  // matching the real event server's wait_for_executors_closed behavior.
  while (true) {
    std::scoped_lock lock(mtx_);
    if (executor_handle_refcounts_[block_id] <= 0) {
      break;
    }
  }

  std::scoped_lock lock(mtx_);
  if (!block_refcounts_.contains(block_id)) {
    error("Block {} is not open", block_id);
    return EventBlockResult(RPCError::kRPCBadBlockID);
  }

  if (block_refcounts_.at(block_id) == 0) {
    error("Block {} is not open", block_id);
    return EventBlockResult(RPCError::kRPCBadBlockID);
  }

  block_refcounts_[block_id]--;
  SHA256Hash root_hash{}; // TODO(geoff): Implement
  EventBlockResult result(RPCError::kRPCNone);
  result.root_hash() = root_hash;
  info("Closed block {}, height = {}", block_id, block_height);
  return result;
}

void
TestEventAggregator::executor_open_block(BlockID block_id)
{
  std::scoped_lock lock(mtx_);
  executor_handle_refcounts_[block_id]++;
}

void
TestEventAggregator::executor_close_block(BlockID block_id)
{
  std::scoped_lock lock(mtx_);
  executor_handle_refcounts_[block_id]--;
}

RPCError
TestEventAggregator::do_emit_event(BlockID block_id, TransactionID tx_id,
                                   TxEventLog event_log)
{
  std::scoped_lock lock(mtx_);
  if (!is_block_open(block_id)) {
    error("Block {} is not open", block_id);
    return RPCError::kRPCBadBlockID;
  }
  if (events_[block_id].contains(tx_id)) {
    error("Unrecoverable: Tx {} already exists for block {}", tx_id, block_id);
    return RPCError::kRPCUnrecoverable;
  }
  events_[block_id][tx_id] = std::move(event_log);
  return RPCError::kRPCNone;
}

x23::Async<RPCError>
TestEventAggregator::emit_event(BlockID block_id, TransactionID tx_id,
                                TxEventLog event_log)
{
  co_await should_block_futex_.wait(true);
  co_return do_emit_event(block_id, tx_id, std::move(event_log));
}

std::optional<TxEventLog>
TestEventAggregator::get_events(BlockID block_id, TransactionID tx_id)
{
  std::scoped_lock lock(mtx_);
  auto it = events_.find(block_id);
  if (it == events_.end()) {
    error("Block {} not found", block_id);
    return std::nullopt;
  }

  if (!it->second.contains(tx_id)) {
    error("Tx {} not found in block {}", tx_id, block_id);
    return std::nullopt;
  }
  return it->second.at(tx_id);
}

std::optional<TxEventLog>
TestEventAggregator::get_events_by_hash(BlockID block_id,
                                        SHA256Hash const &tx_hash)
{
  std::scoped_lock lock(mtx_);
  auto it = events_.find(block_id);
  if (it == events_.end()) {
    error("Block {} not found", block_id);
    return std::nullopt;
  }

  for (auto const &[tx_id, event_log] : it->second) {
    if (event_log.proposed_tx_hash == tx_hash) {
      return event_log;
    }
  }
  error("Tx hash not found in block {}", block_id);
  return std::nullopt;
}

} // namespace hogd
