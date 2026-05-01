#pragma once

#include "event.hh"
#include "event_rpc.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

struct DevEventAggregatorExecRpc {
  using rpc_service = EventExecutor1;

  static void eventexecutor_null() {}

  static x23::Async<RPCError> eventexecutor_open_block(BlockID block_id)
  {
    co_return RPCError::kRPCNone;
  }

  static x23::Async<RPCError> eventexecutor_close_block(BlockID block_id)
  {
    co_return RPCError::kRPCNone;
  }

  static x23::Async<RPCError>
  eventexecutor_emit_event(BlockID block_id, TransactionID transaction_id,
                           TxEventLog const &event)
  {
    co_return RPCError::kRPCNone;
  }
};
static_assert(is_EventExecutor1<DevEventAggregatorExecRpc *, x23::MaybeAsync>);

class DevEventAggregatorExecServer
  : public RpcServer<DevEventAggregatorExecRpc> {};

struct DevEventAggregatorSchedRpc {
  using rpc_service = EventSched1;

  static void eventsched_null() {}

  static x23::Async<RPCError> eventsched_open_block(BlockID block_id)
  {
    co_return RPCError::kRPCNone;
  }

  static x23::Async<EventBlockResult>
  eventsched_close_block(BlockID block_id, BlockHeight block_height)
  {
    co_return EventBlockResult(RPCError::kRPCNone);
  }

  static x23::Async<RPCError> eventsched_abandon_block(BlockID block_id)
  {
    co_return RPCError::kRPCNone;
  }
};
static_assert(is_EventSched1<DevEventAggregatorSchedRpc *, x23::MaybeAsync>);

class DevEventAggregatorSchedServer
  : public RpcServer<DevEventAggregatorSchedRpc> {};

} // namespace hogd
