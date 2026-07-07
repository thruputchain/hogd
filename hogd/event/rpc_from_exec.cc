#include "hogd/event/rpc_from_exec.h"

#include <utility>

#include "db_conflict.hh"
#include "event.hh"
#include "hogd/base/log.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"

namespace hogd {

x23::Async<RPCError>
EventExecRpc::eventexecutor_open_block(BlockID block_id)
{
  info("eventexecutor_open_block: block_id={}", block_id);
  auto context = co_await ctx_.get_context(block_id);
  if (!context) {
    info("eventexecutor_open_block: block_id={} bad_block_id", block_id);
    co_return RPCError::kRPCBadBlockID;
  }

  context->executor_open_block();

  if (context.release_reference()) [[unlikely]] {
    co_await ctx_.gc_context(block_id);
  }

  info("eventexecutor_open_block: block_id={} done", block_id);
  co_return RPCError::kRPCNone;
}

x23::Async<RPCError>
EventExecRpc::eventexecutor_close_block(BlockID block_id)
{
  info("eventexecutor_close_block: block_id={}", block_id);
  auto context = co_await ctx_.get_context(block_id);
  if (!context) {
    info("eventexecutor_close_block: block_id={} bad_block_id", block_id);
    co_return RPCError::kRPCBadBlockID;
  }

  context->executor_close_block();

  if (context.release_reference()) [[unlikely]] {
    co_await ctx_.gc_context(block_id);
  }

  info("eventexecutor_close_block: block_id={} done", block_id);
  co_return RPCError::kRPCNone;
}

x23::Async<RPCError>
EventExecRpc::eventexecutor_emit_event(BlockID block_id,
                                       TransactionID /*unused*/,
                                       TxEventLog event_log,
                                       ProposedTransaction proposed_tx)
{
  auto context = co_await ctx_.get_context(block_id);
  if (!context) {
    co_return RPCError::kRPCBadBlockID;
  }

  // The body is accumulated alongside the event log and forwarded to the
  // indexer when the block commits (see send_events_to_indexers).
  bool ok =
      co_await context->add_event(std::move(event_log), std::move(proposed_tx));

  if (context.release_reference()) [[unlikely]] {
    co_await ctx_.gc_context(block_id);
  }

  co_return ok ? RPCError::kRPCNone : RPCError::kRPCBadBlockID;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
x23::Async<RPCError>
EventExecRpc::eventexecutor_report_tx_failure(SignedTransactionHash /*tx_hash*/,
                                              TxFailureDiagnostic /*diag*/)
{
  // Relay to registered indexers lands in a later commit; accept and drop.
  co_return RPCError::kRPCNone;
}

} // namespace hogd
