#include "hogd/hogexec/interface/rpc_from_coord.h"

#include <atomic>
#include <cassert>
#include <memory>

#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/interface/executor_context.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"

namespace hogd {

std::shared_ptr<ExecutorContext>
ExecutorMempoolRpc::try_get_context(BlockID block_id, ExecutionMode mode)
{
  if (!initialize_check()) [[unlikely]] {
    warn("ExecutorMempoolRpc: accessing context before initialization");
    return nullptr;
  }

  auto ctx = executor_states_->get_executor_context(block_id);
  if (!ctx) {
    return nullptr;
  }
  if (ctx->mode() != mode) {
    warn("Expected executor context for block {} to be in {} mode, but found "
         "in {} mode",
         block_id, mode, ctx->mode());
    return nullptr;
  }

  return ctx;
}

x23::Async<ExecProposeResult>
ExecutorMempoolRpc::executorsched_propose_transaction(
    BlockID block_id, SignedTransaction signed_tx)
{

  auto ctx = try_get_context(block_id, ExecutionMode::kProposing);
  if (!ctx) {
    co_return ExecProposeResult(ExecResult::ExecResultSystemError);
  }

  co_return co_await ctx->propose_transaction(signed_tx, allocate_tx_ids(1));
}

x23::Async<ExecResult>
ExecutorMempoolRpc::executorsched_validate_transaction(
    BlockID block_id, ProposedTransaction proposed_tx)
{
  auto ctx = try_get_context(block_id, ExecutionMode::kValidating);
  if (!ctx) {
    co_return ExecResult::ExecResultSystemError;
  }

  co_return co_await ctx->validate_transaction(proposed_tx, allocate_tx_ids(1));
}

x23::Async<ExecProposeBatchResult>
ExecutorMempoolRpc::executorsched_propose_batch(
    BlockID block_id, SignedTransactionBatch signed_txs)
{
  auto ctx = try_get_context(block_id, ExecutionMode::kProposing);
  if (!ctx) {
    co_return ExecProposeBatchResult(signed_txs.size(),
                                     ExecProposeResult(
                                         ExecResult::ExecResultSystemError));
  }

  co_return co_await ctx->propose_transaction_batch(signed_txs,
                                                    allocate_tx_ids(
                                                        signed_txs.size()));
}

x23::Async<ExecValidateBatchResult>
ExecutorMempoolRpc::executorsched_validate_batch(
    BlockID block_id, ProposedTransactionBatch proposed_txs)
{
  auto ctx = try_get_context(block_id, ExecutionMode::kValidating);
  if (!ctx) {
    co_return ExecValidateBatchResult(proposed_txs.size(),
                                      ExecResult::ExecResultSystemError);
  }

  co_return co_await ctx->validate_transaction_batch(proposed_txs,
                                                     allocate_tx_ids(
                                                         proposed_txs.size()));
}

ExecResult
ExecutorMempoolRpc::executorsched_open_proposal_block(BlockID id)
{
  if (!initialize_check()) [[unlikely]] {
    return ExecResult::ExecResultSystemError;
  }
  return executor_states_->open_proposal(id);
}

ExecResult
ExecutorMempoolRpc::executorsched_open_validation_block(BlockID id)
{
  if (!initialize_check()) [[unlikely]] {
    return ExecResult::ExecResultSystemError;
  }
  return executor_states_->open_validation(id);
}

void
ExecutorMempoolRpc::executorsched_finish_block(BlockID id)
{
  if (!initialize_check()) [[unlikely]] {
    return;
  }
  executor_states_->remove_block(id);
}

TransactionID
ExecutorMempoolRpc::allocate_tx_ids(size_t count)
{
  uint64_t base = self_id_.fetch_add(count, std::memory_order_relaxed);
  assert(base + count - 1 < uint64_t{1} << 48); // another executor's id block
  return TransactionID{base + (uint64_t{self_executor_id_} << 48)};
}

x23::Async<void>
ExecutorMempoolRpc::executorsched_shutdown()
{
  shut_down();
  co_return;
}

} // namespace hogd
