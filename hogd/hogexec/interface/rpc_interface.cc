#include "hogd/hogexec/interface/rpc_interface.h"

#include <atomic>
#include <cassert>

#include "exec_coord.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"

namespace hogd {

x23::Async<ExecProposeResult>
ExecutorRpcInterface::executorcoord_propose_transaction(
    BlockID block_id, SignedTransaction const &signed_tx)
{
  auto ctx = executor_states_.get_executor_context(block_id,
                                                   ExecutionMode::kProposing);
  if (!ctx) {
    co_return ExecProposeResult(ExecResult::ExecResultSystemError);
  }

  co_return co_await ctx->propose_transaction(signed_tx, allocate_tx_id());
}

x23::Async<ExecResult>
ExecutorRpcInterface::executorcoord_validate_transaction(
    BlockID block_id, ProposedTransaction const &proposed_tx)
{
  auto ctx = executor_states_.get_executor_context(block_id,
                                                   ExecutionMode::kValidating);
  if (!ctx) {
    co_return ExecResult::ExecResultSystemError;
  }

  co_return co_await ctx->validate_transaction(proposed_tx, allocate_tx_id());
}

x23::Async<ExecResult>
ExecutorRpcInterface::executorcoord_open_proposal_block(BlockID id)
{
  executor_states_.open_proposal(id, main_worker_pool_);
  co_return ExecResult::ExecResultOK;
}

x23::Async<ExecResult>
ExecutorRpcInterface::executorcoord_open_validation_block(BlockID id)
{
  executor_states_.open_validation(id, main_worker_pool_);
  co_return ExecResult::ExecResultOK;
}

x23::Async<void>
ExecutorRpcInterface::executorcoord_finish_block(BlockID id)
{
  executor_states_.remove_block(id);
  co_return;
}

TransactionID
ExecutorRpcInterface::allocate_tx_id()
{
  uint64_t base = self_id_.fetch_add(1, std::memory_order_relaxed);
  return TransactionID{base + (uint64_t{self_executor_id_} << 48)};
}

x23::Async<void>
ExecutorRpcInterface::executorcoord_shutdown()
{
  shutdown_flag_ = true;
  co_return;
}

} // namespace hogd
