#pragma once

#include <atomic>
#include <memory>
#include <utility>

#include "exec_coord.hh"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/hogexec/interface/executor_state_manager.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"

namespace hogd {

// Calls from coordinator into executor
class ExecutorRpcInterface {
public:
  using rpc_service = ExecutorCoord1;

  ExecutorRpcInterface(uint16_t self_executor_id, WorkerPool &main_worker_pool,
                       std::unique_ptr<DatabaseProxy> db_proxy)
    : self_executor_id_(self_executor_id), main_worker_pool_(main_worker_pool),
      executor_states_(std::move(db_proxy))
  {}

  // Disable copy and move
  ExecutorRpcInterface(ExecutorRpcInterface const &) = delete;
  ExecutorRpcInterface &operator=(ExecutorRpcInterface const &) = delete;
  ExecutorRpcInterface(ExecutorRpcInterface &&) = delete;
  ExecutorRpcInterface &operator=(ExecutorRpcInterface &&) = delete;

  static x23::Async<void> executorcoord_null() { co_return; }

  x23::Async<ExecProposeResult>
  executorcoord_propose_transaction(BlockID block_id,
                                    SignedTransaction const &signed_tx);
  x23::Async<ExecResult>
  executorcoord_validate_transaction(BlockID block_id,
                                     ProposedTransaction const &proposed_tx);

  x23::Async<ExecResult> executorcoord_open_proposal_block(BlockID id);
  x23::Async<ExecResult> executorcoord_open_validation_block(BlockID id);
  // Indicates that state associated with this block can be garbage collected.
  // Does not block.  State will not be garbage collected until all transactions
  // on this block have been committed or aborted.
  x23::Async<void> executorcoord_finish_block(BlockID id);

  // It is an error to shut down the executor while there are transactions
  // that are still running.
  x23::Async<void> executorcoord_shutdown();

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  TransactionID allocate_tx_id();

  std::atomic<uint64_t> self_id_ = 0;
  const uint16_t self_executor_id_;

  WorkerPool &main_worker_pool_;
  ExecutorStateManager executor_states_;
  std::atomic<bool> shutdown_flag_ = false;
};

static_assert(is_ExecutorCoord1<ExecutorRpcInterface *, x23::Async>);

} // namespace hogd
