#include "hogd/hogexec/interface/rpc_interface.h"

namespace hogd {

x23::Async<ExpectedProposalData>
ExecutorRpcInterface::propose_transaction(BlockID block_id,
                                          SignedTransaction const &signed_tx)
{
  auto *ctx = executor_states_.get_executor_context(block_id);
  if (!ctx) {
    co_return ExpectedProposalData(RPCError::kBadExecutorState);
  }

  if (ctx->state_.try_acquire_run_lease(ExecutorMode::kProposing)) {
    // TODO: invoke the transaction, and so on and so forth
    // allocate transaction id here
    throw std::runtime_error("unimplemented");
  }
  co_return ExpectedProposalData(RPCError::kBadExecutorState);
}

x23::Async<RPCError>
ExecutorRpcInterface::validate_transaction(
    BlockID block_id, ProposedTransaction const &proposed_tx)
{
  auto *ctx = executor_states_.get_executor_context(block_id);
  if (!ctx) {
    co_return RPCError::kBadExecutorState;
  }

  if (ctx->state_.try_acquire_run_lease(ExecutorMode::kValidating)) {
    // TODO: invoke the transaction, and so on and so forth
    throw std::runtime_error("unimplemented");
  }
  co_return RPCError::kBadExecutorState;
}

x23::Async<void>
ExecutorRpcInterface::open_proposal_block(BlockID id)
{
  executor_states_.open_proposal(id);
  co_return;
}

x23::Async<void>
ExecutorRpcInterface::open_validation_block(BlockID id)
{
  executor_states_.open_validation(id);
  co_return;
}

x23::Async<void>
ExecutorRpcInterface::finish_block(BlockID id)
{
  executor_states_.await_finish(id);
  co_return;
}

} // namespace hogd
