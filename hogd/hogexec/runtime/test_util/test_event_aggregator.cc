#include "hogd/hogexec/runtime/test_util/test_event_aggregator.h"

#include <mutex>
#include <optional>

#include "event.hh"
#include "hogd/base/types.h"
#include "rpc_types.hh"
#include "x23/rpc.h"

namespace hogd {

RPCError
TestEventAggregator::executorevent_open_block(BlockID block_id)
{
  std::lock_guard<std::mutex> lock(mtx_);
  block_refcounts_[block_id]++;
  return RPCError::kRPCNone;
}

RPCError
TestEventAggregator::executorevent_close_block(BlockID block_id)
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (!block_refcounts_.contains(block_id)) {
    return RPCError::kRPCBadBlockID;
  }

  if (block_refcounts_.at(block_id) == 0) {
    return RPCError::kRPCBadBlockID;
  }

  block_refcounts_[block_id]--;
  return RPCError::kRPCNone;
}

RPCError
TestEventAggregator::executorevent_emit_event(BlockID block_id,
                                              TransactionID tx_id,
                                              TxEventLog const &event_log)
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (!is_block_open(block_id)) {
    return RPCError::kRPCBadBlockID;
  }

  if (events_[block_id].contains(tx_id)) {
    return RPCError::kRPCUnrecoverable;
  }

  events_[block_id][tx_id] = event_log;
  return RPCError::kRPCNone;
}

std::optional<TxEventLog>
TestEventAggregator::get_events(BlockID block_id, TransactionID tx_id)
{
  std::lock_guard<std::mutex> lock(mtx_);
  auto it = events_.find(block_id);
  if (it == events_.end()) {
    return std::nullopt;
  }

  if (!it->second.contains(tx_id)) {
    return std::nullopt;
  }
  return it->second.at(tx_id);
}

uint16_t
TestEventAggregator::start_server(uint16_t port_number)
{
  return tcp_rpc_server(cp_, port_number, [this](auto c) { c->server(this); });
}

} // namespace hogd
