#pragma once

#include "db_conflict.hh"
#include "event.hh"
#include "event_rpc.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/hog_event/rpc_context.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"

namespace hogd {

class EventExecRpc {
public:
  using rpc_service = EventExecutor1;

  EventExecRpc(EventRPCContext &ctx) : ctx_(ctx) {}

  static void eventexecutor_null() {}

  x23::Async<RPCError> eventexecutor_open_block(BlockID block_id);
  x23::Async<RPCError> eventexecutor_close_block(BlockID block_id);

  x23::Async<RPCError>
  eventexecutor_emit_event(BlockID block_id, TransactionID tx_id,
                           TxEventLog event_log,
                           ProposedTransaction proposed_tx);

  x23::Async<RPCError>
  eventexecutor_report_tx_failure(TransactionHash tx_hash,
                                  TxFailureDiagnostic diag);

private:
  EventRPCContext &ctx_;
};

static_assert(is_EventExecutor1<EventExecRpc *, x23::MaybeAsync>);

class EventExecServer : public RpcServer<EventExecRpc> {
public:
  explicit EventExecServer(EventRPCContext &ctx) : RpcServer(ctx) {}
};

} // namespace hogd
