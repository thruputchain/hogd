#pragma once

#include "hogd/base/rpc_server.h"
#include "hogd/event/rpc_context.h"
#include "hogd/version.h"
#include "indexer.hh"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

class EventIndexerRpc {
public:
  using rpc_service = EventIndexer1;

  EventIndexerRpc(EventRPCContext &ctx) : ctx_(ctx) {}

  // TODO(jhumphri): Should the indexer instead register with the mempool, which
  // in turn will send the indexer's address to the event server?
  x23::Async<void> event_indexer_intro(IndexerCard card)
  {
    CHECK_EQ(strcmp(card.protocol.c_str(), version::kHogdProtocol()), 0);
    indexer_event_client_t client = co_await make_rpc_client<IndexerEvent1>(
        server_address_to_sockaddr(card.address_for_event), /*retry=*/false);
    co_await ctx_.add_indexer(std::move(client));
    co_return;
  }

private:
  EventRPCContext &ctx_;
};
static_assert(is_EventIndexer1<EventIndexerRpc *, x23::MaybeAsync>);

class EventIndexerServer : public RpcServer<EventIndexerRpc> {
public:
  explicit EventIndexerServer(EventRPCContext &ctx) : RpcServer(ctx) {}
};

} // namespace hogd
