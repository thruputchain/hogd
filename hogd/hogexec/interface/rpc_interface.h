#pragma once

#include "hogd/hogexec/interface/executor_state.h"
#include "rpc.hh"
#include "transaction.hh"
#include "x23/async.h"

namespace hogd {

// Calls from coordinator into executor
class ExecutorRpcInterface {
public:
  using rpc_service = ExecutorInterfaceV1;

  ExecutorRpcInterface(ExecutorRpcInterface const &) = delete;
  ExecutorRpcInterface &operator=(ExecutorRpcInterface const &) = delete;
  ExecutorRpcInterface(ExecutorRpcInterface &&) = delete;
  ExecutorRpcInterface &operator=(ExecutorRpcInterface &&) = delete;

  x23::Async<ExpectedProposalData>
  propose_transaction(SignedTransaction const &signed_tx);
  x23::Async<RPCError>
  validate_transaction(ProposedTransaction const &proposed_tx);

  x23::Async<void> open_proposal_block();
  x23::Async<void> open_validation_block();
  x23::Async<void> finish_block();

private:
  ExecutorState executor_state_;
  std::mutex coord_control_mutex_;
};

static_assert(is_ExecutorInterfaceV1<ExecutorRpcInterface *, x23::Async>);

} // namespace hogd
