#pragma once

#include <atomic>
#include <memory>
#include <utility>

#include "exec_coord.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/interface/executor_state_manager.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/event.h"

namespace hogd {

// Calls from coordinator into executor
class ExecutorRpcInterface {
public:
  using rpc_service = ExecutorCoord1;

  ExecutorRpcInterface(uint16_t self_executor_id, x23::CoroPoll &cp,
                       std::unique_ptr<DatabaseProxy> db_proxy)
    : self_executor_id_(self_executor_id), cp_(cp),
      executor_states_(std::move(db_proxy))
  {}

  uint16_t start_server(uint16_t port_number);

  // Disable copy and move
  ExecutorRpcInterface(ExecutorRpcInterface const &) = delete;
  ExecutorRpcInterface &operator=(ExecutorRpcInterface const &) = delete;
  ExecutorRpcInterface(ExecutorRpcInterface &&) = delete;
  ExecutorRpcInterface &operator=(ExecutorRpcInterface &&) = delete;

  static x23::Async<void> executorcoord_null() { co_return; }

  x23::Async<ExecProposeResult>
  executorcoord_propose_transaction(BlockID block_id,
                                    SignedTransaction signed_tx);
  x23::Async<ExecResult>
  executorcoord_validate_transaction(BlockID block_id,
                                     ProposedTransaction proposed_tx);

  x23::Async<ExecProposeBatchResult>
  executorcoord_propose_batch(BlockID block_id,
                              SignedTransactionBatch signed_txs);
  x23::Async<ExecValidateBatchResult>
  executorcoord_validate_batch(BlockID block_id,
                               ProposedTransactionBatch proposed_txs);

  ExecResult executorcoord_open_proposal_block(BlockID id);
  ExecResult executorcoord_open_validation_block(BlockID id);
  // Indicates that state associated with this block can be garbage collected.
  // Does not block.  State will not be garbage collected until all transactions
  // on this block have been committed or aborted.
  void executorcoord_finish_block(BlockID id);

  // It is an error to shut down the executor while there are transactions
  // that are still running.
  x23::Async<void> executorcoord_shutdown();

  void shut_down();
  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  TransactionID allocate_tx_id();

  std::atomic<uint64_t> self_id_ = 0;
  const uint16_t self_executor_id_;

  x23::CoroPoll &cp_;

  ExecutorStateManager executor_states_;
  std::atomic<bool> shutdown_flag_ = false;
};

static_assert(is_ExecutorCoord1<ExecutorRpcInterface *, x23::MaybeAsync>);

} // namespace hogd
