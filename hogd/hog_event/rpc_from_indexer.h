#pragma once

#include "hogd/base/rpc_server.h"
#include "hogd/hog_event/rpc_context.h"
#include "hogd/xdr/indexer_payload_validation.h"
#include "indexer.hh"
#include "x23/async.h"

namespace hogd {

class EventIndexerRpc {
public:
  using rpc_service = EventIndexer1;

  EventIndexerRpc(EventRPCContext &ctx) : ctx_(ctx) {}

  x23::Async<EventIndexerPullResult>
  event_indexer_pull_next(EventIndexerPullRequest request)
  {
    co_return co_await ctx_.pull_indexer_next(request);
  }

  IndexerTxFailureNoticeBatch event_indexer_pull_tx_failures()
  {
    return ctx_.pull_tx_failures();
  }

private:
  EventRPCContext &ctx_;
};
static_assert(is_EventIndexer1<EventIndexerRpc *, x23::MaybeAsync>);

class EventIndexerServer : public RpcServer<EventIndexerRpc> {
public:
  explicit EventIndexerServer(EventRPCContext &ctx) : RpcServer(ctx), ctx_(ctx)
  {
    max_message_size_ = kIndexerRpcMaxMessageSize;
  }

  // ~RpcServer waits for in-flight handlers but cannot cancel a suspended one
  // (RpcServerShutdown.SuspendedIndexerPullPinsServerDestruction). A pull
  // parked on the next publication is exactly that, so release them here,
  // before the base destructor drains.
  ~EventIndexerServer() { ctx_.release_indexer_pulls(); }

  // Disable copy and move.
  EventIndexerServer(const EventIndexerServer &) = delete;
  EventIndexerServer &operator=(const EventIndexerServer &) = delete;
  EventIndexerServer(EventIndexerServer &&) = delete;
  EventIndexerServer &operator=(EventIndexerServer &&) = delete;

private:
  EventRPCContext &ctx_;
};

} // namespace hogd
