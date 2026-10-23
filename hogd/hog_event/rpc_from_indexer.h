#pragma once

#include <atomic>

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
    co_return co_await ctx_.pull_indexer_next(request, draining_);
  }

  IndexerTxFailureNoticeBatch event_indexer_pull_tx_failures()
  {
    return ctx_.pull_tx_failures();
  }

  x23::Async<EventIndexerChunkedPullResult>
  event_indexer_pull_next_chunked(EventIndexerPullRequest request)
  {
    co_return co_await ctx_.pull_indexer_next_chunked(request, draining_);
  }

  x23::Async<EventIndexerBlockPairsResult>
  event_indexer_pull_block_pairs(EventIndexerBlockPairsRequest request)
  {
    co_return co_await ctx_.pull_indexer_block_pairs(request);
  }

  // Set by ~EventIndexerServer before it releases parked pulls. From then on
  // a pull that would park is answered, whichever side of the release it
  // arrives on.
  void mark_draining() { draining_.store(true, std::memory_order_release); }

private:
  EventRPCContext &ctx_;
  std::atomic<bool> draining_ = false;
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
  // before the base destructor drains. The flag goes first: a client at the
  // tip re-asks the moment its released pull is answered, and that pull
  // must be answered too rather than parked on the post-release count.
  ~EventIndexerServer()
  {
    impl()->mark_draining();
    ctx_.release_indexer_pulls();
  }

  // Disable copy and move.
  EventIndexerServer(const EventIndexerServer &) = delete;
  EventIndexerServer &operator=(const EventIndexerServer &) = delete;
  EventIndexerServer(EventIndexerServer &&) = delete;
  EventIndexerServer &operator=(EventIndexerServer &&) = delete;

private:
  EventRPCContext &ctx_;
};

} // namespace hogd
