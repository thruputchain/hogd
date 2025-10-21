#include "hogd/event/rpc_from_exec.h"

#include "event.hh"
#include "rpc_types.hh"

namespace hogd {

RPCError
EventExecRpc::eventexecutor_emit_event(BlockID block_id,
                                       TransactionID /*unused*/,
                                       TxEventLog const &event_log)
{
  auto *context = ctx_.get_context(block_id);
  if (!context) {
    return RPCError::kRPCBadBlockID;
  }

  context->get().add_event(event_log);

  if (context->release_reference()) [[unlikely]] {
    ctx_.gc_context(block_id);
  }

  return RPCError::kRPCNone;
}

} // namespace hogd
