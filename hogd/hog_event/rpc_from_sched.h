#pragma once

#include "consensus.hh"
#include "event_rpc.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/hog_event/rpc_context.h"
#include "proposal.hh"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

class EventMempoolRpc {
public:
  using rpc_service = EventSched1;

  EventMempoolRpc(EventRPCContext &ctx) : ctx_(ctx) {}

  static void eventsched_null() {}

  x23::Async<RPCError> eventsched_open_block(BlockID block_id);
  x23::Async<EventBlockResult> eventsched_close_block(BlockID block_id,
                                                      BlockHeight block_height,
                                                      ProposalTxs proposal_txs);
  x23::Async<RPCError> eventsched_commit_block(BlockID block_id,
                                               ConsensusValue consensus_value);
  x23::Async<RPCError>
  eventsched_confirm_committed_block(ConsensusValue consensus_value);
  x23::Async<RPCError> eventsched_abandon_block(BlockID block_id);

private:
  EventRPCContext &ctx_;
};

static_assert(is_EventSched1<EventMempoolRpc *, x23::MaybeAsync>);

class EventMempoolServer : public RpcServer<EventMempoolRpc> {
public:
  explicit EventMempoolServer(EventRPCContext &ctx) : RpcServer(ctx) {}
};

} // namespace hogd
