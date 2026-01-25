#include "hogd/event/rpc_from_exec.h"

#include "event.hh"
#include "rpc_types.hh"
#include "x23/async.h"

namespace hogd {

x23::Async<RPCError>
EventExecRpc::eventexecutor_emit_event(BlockID block_id,
                                       TransactionID /*unused*/,
                                       TxEventLog event_log)
{
  auto *context = co_await ctx_.get_context(block_id);
  if (!context) {
    co_return RPCError::kRPCBadBlockID;
  }

  context->get().add_event(event_log);

  if (context->release_reference()) [[unlikely]] {
    co_await ctx_.gc_context(block_id);
  }

  co_return RPCError::kRPCNone;
}

} // namespace hogd
