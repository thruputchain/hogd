#pragma once

#include "hogd/hogexec/interface/executor_state.h"
#include "hogd/hogexec/interface/executor_state_manager.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "rpc.hh"
#include "transaction.hh"
#include "x23/async.h"

namespace hogd {

// Calls from coordinator into executor
class ExecutorRpcInterface {
public:
  using rpc_service = ExecutorInterfaceV1;

  ExecutorRpcInterface(DatabaseProxy &db_proxy) : executor_states_(db_proxy) {}

  // Disable copy and move
  ExecutorRpcInterface(ExecutorRpcInterface const &) = delete;
  ExecutorRpcInterface &operator=(ExecutorRpcInterface const &) = delete;
  ExecutorRpcInterface(ExecutorRpcInterface &&) = delete;
  ExecutorRpcInterface &operator=(ExecutorRpcInterface &&) = delete;

  x23::Async<ExpectedProposalData>
  propose_transaction(BlockID block_id, SignedTransaction const &signed_tx);
  x23::Async<RPCError>
  validate_transaction(BlockID block_id,
                       ProposedTransaction const &proposed_tx);

  x23::Async<void> open_proposal_block(BlockID id);
  x23::Async<void> open_validation_block(BlockID id);
  x23::Async<void> finish_block(BlockID id);

private:
  ExecutorStateManager executor_states_;
};

static_assert(is_ExecutorInterfaceV1<ExecutorRpcInterface *, x23::Async>);

} // namespace hogd
