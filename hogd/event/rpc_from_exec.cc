#include "hogd/event/rpc_from_exec.h"

#include <utility>

#include "event.hh"
#include "rpc_types.hh"
#include "x23/async.h"

namespace hogd {

x23::Async<RPCError>
EventExecRpc::eventexecutor_open_block(BlockID block_id)
{
  auto context = co_await ctx_.get_context(block_id);
  if (!context) {
    co_return RPCError::kRPCBadBlockID;
  }

  context->executor_open_block();

  if (context.release_reference()) [[unlikely]] {
    co_await ctx_.gc_context(block_id);
  }

  co_return RPCError::kRPCNone;
}

x23::Async<RPCError>
EventExecRpc::eventexecutor_close_block(BlockID block_id)
{
  auto context = co_await ctx_.get_context(block_id);
  if (!context) {
    co_return RPCError::kRPCBadBlockID;
  }

  context->executor_close_block();

  if (context.release_reference()) [[unlikely]] {
    co_await ctx_.gc_context(block_id);
  }

  co_return RPCError::kRPCNone;
}

x23::Async<RPCError>
EventExecRpc::eventexecutor_emit_event(BlockID block_id,
                                       TransactionID /*unused*/,
                                       TxEventLog event_log)
{
  auto context = co_await ctx_.get_context(block_id);
  if (!context) {
    co_return RPCError::kRPCBadBlockID;
  }

  co_await context->add_event(std::move(event_log));

  if (context.release_reference()) [[unlikely]] {
    co_await ctx_.gc_context(block_id);
  }

  co_return RPCError::kRPCNone;
}

} // namespace hogd
