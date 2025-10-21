#pragma once

#include <type_traits>

#include "event_rpc.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/event/rpc_context.h"
#include "rpc_types.hh"

namespace hogd {

class EventSchedRpc {
public:
  using rpc_service = EventSched1;

  EventSchedRpc(EventRPCContext &ctx) : ctx_(ctx) {}

  static void eventsched_null() {}

  RPCError eventsched_open_block(BlockID block_id);
  EventBlockResult eventsched_close_block(BlockID block_id);
  RPCError eventsched_abandon_block(BlockID block_id);

private:
  EventRPCContext &ctx_;
};

static_assert(is_EventSched1<EventSchedRpc *, std::type_identity_t>);

class EventSchedServer : public RpcServer<EventSchedRpc> {
public:
  explicit EventSchedServer(EventRPCContext &ctx) : RpcServer(ctx) {}
};

} // namespace hogd
