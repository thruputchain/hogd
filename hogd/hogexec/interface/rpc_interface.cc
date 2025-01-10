#include "hogd/hogexec/interface/rpc_interface.h"

namespace hogd {

x23::Async<ExpectedProposalData>
ExecutorRpcInterface::propose_transaction(SignedTransaction const &signed_tx)
{
  if (executor_state_.try_acquire_run_lease(ExecutorMode::kProposing)) {
    // TODO: invoke the transaction, and so on and so forth
    throw std::runtime_error("unimplemented");
  }
  co_return ExpectedProposalData(RPCError::kBadExecutorState);
}

x23::Async<RPCError>
ExecutorRpcInterface::validate_transaction(
    ProposedTransaction const &proposed_tx)
{
  if (executor_state_.try_acquire_run_lease(ExecutorMode::kValidating)) {
    // TODO: invoke the transaction, and so on and so forth
    throw std::runtime_error("unimplemented");
  }
  co_return RPCError::kBadExecutorState;
}

x23::Async<void>
ExecutorRpcInterface::open_proposal_block()
{
  std::lock_guard lock(coord_control_mutex_);
  executor_state_.start_proposing();
  co_return;
}

x23::Async<void>
ExecutorRpcInterface::open_validation_block()
{
  std::lock_guard lock(coord_control_mutex_);
  executor_state_.start_validating();
  co_return;
}

x23::Async<void>
ExecutorRpcInterface::finish_block()
{
  std::lock_guard lock(coord_control_mutex_);
  executor_state_.await_finish();
  co_return;
}

} // namespace hogd
