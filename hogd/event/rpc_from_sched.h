#pragma once

#include "event_rpc.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/event/rpc_context.h"
#include "rpc_types.hh"
#include "x23/async.h"

namespace hogd {

class EventSchedRpc {
public:
  using rpc_service = EventSched1;

  EventSchedRpc(EventRPCContext &ctx) : ctx_(ctx) {}

  static void eventsched_null() {}

  RPCError eventsched_open_block(BlockID block_id);
  x23::Async<EventBlockResult> eventsched_close_block(BlockID block_id);
  RPCError eventsched_abandon_block(BlockID block_id);

private:
  EventRPCContext &ctx_;
};

static_assert(is_EventSched1<EventSchedRpc *, x23::MaybeAsync>);

class EventSchedServer : public RpcServer<EventSchedRpc> {
public:
  explicit EventSchedServer(EventRPCContext &ctx) : RpcServer(ctx) {}
};

} // namespace hogd
