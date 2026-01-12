#pragma once

#include "event.hh"
#include "event_rpc.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/event/rpc_context.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

class EventExecRpc {
public:
  using rpc_service = EventExecutor1;

  EventExecRpc(EventRPCContext &ctx) : ctx_(ctx) {}

  static void eventexecutor_null() {}

  x23::Async<RPCError> eventexecutor_emit_event(BlockID block_id,
                                                TransactionID tx_id,
                                                TxEventLog const &event_log);

private:
  EventRPCContext &ctx_;
};

static_assert(is_EventExecutor1<EventExecRpc *, x23::MaybeAsync>);

class EventExecServer : public RpcServer<EventExecRpc> {
public:
  explicit EventExecServer(EventRPCContext &ctx) : RpcServer(ctx) {}
};

} // namespace hogd
