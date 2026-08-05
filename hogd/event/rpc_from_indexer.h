#pragma once

#include "hogd/base/rpc_server.h"
#include "hogd/event/rpc_context.h"
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
  explicit EventIndexerServer(EventRPCContext &ctx) : RpcServer(ctx)
  {
    max_message_size_ = kIndexerRpcMaxMessageSize;
  }
};

} // namespace hogd
