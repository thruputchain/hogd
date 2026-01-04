#include "hogd/hogexec/runtime/test_util/test_event_aggregator.h"

#include <mutex>
#include <optional>

#include "event.hh"
#include "event_rpc.hh"
#include "hogd/base/log.h"
#include "rpc_types.hh"
#include "types.hh"

namespace hogd {

RPCError
TestEventAggregator::open_block(BlockID block_id)
{
  info("Opening block {}", block_id);
  std::lock_guard<std::mutex> lock(mtx_);
  block_refcounts_[block_id]++;
  return RPCError::kRPCNone;
}

EventBlockResult
TestEventAggregator::close_block(BlockID block_id)
{
  std::lock_guard<std::mutex> lock(mtx_);
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
  info("Closed block {}", block_id);
  return result;
}

RPCError
TestEventAggregator::emit_event(BlockID block_id, TransactionID tx_id,
                                TxEventLog const &event_log)
{
  std::unique_lock<std::mutex> lock(mtx_);
  std::println("Emitting event for block {} and tx {}", block_id, tx_id);
  // Block if should_block_ is true
  emit_cv_.wait(lock, [this] { return !should_block_; });

  std::println("Post wait: Emitting event for block {} and tx {}", block_id, tx_id);
  if (!is_block_open(block_id)) {
    error("Block {} is not open", block_id);
    return RPCError::kRPCBadBlockID;
  }

  if (events_[block_id].contains(tx_id)) {
    error("Unrecoverable: Tx {} already exists for block {}", tx_id, block_id);
    return RPCError::kRPCUnrecoverable;
  }

  events_[block_id][tx_id] = event_log;
  return RPCError::kRPCNone;
}

std::optional<TxEventLog>
TestEventAggregator::get_events(BlockID block_id, TransactionID tx_id)
{
  std::lock_guard<std::mutex> lock(mtx_);
  std::println("Getting events for block {} and tx {}", block_id, tx_id);
  auto it = events_.find(block_id);
  if (it == events_.end()) {
    std::println("Block {} not found", block_id);
    return std::nullopt;
  }

  if (!it->second.contains(tx_id)) {
    std::println("Tx {} not found", tx_id);
    return std::nullopt;
  }
  return it->second.at(tx_id);
}

} // namespace hogd
