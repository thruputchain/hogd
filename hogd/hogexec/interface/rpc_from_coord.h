#pragma once

#include <atomic>
#include <memory>
#include <utility>

#include "exec_sched.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/interface/executor_state_manager.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/event.h"

namespace hogd {

// Calls from coordinator into executor
class ExecutorMempoolRpc {
public:
  using rpc_service = ExecutorSched1;

  ExecutorMempoolRpc() {}

  void init(IntroExecResult &&exec_result)
  {
    self_executor_id_ = exec_result.exec_id;

    DatabaseIdentifier identifier{ShardedDB(exec_result)};
    auto db_proxy = std::make_unique<DatabaseProxy>(std::move(identifier));
    executor_states_ =
        std::make_unique<ExecutorStateManager>(std::move(db_proxy));
  }

  // Disable copy and move
  ExecutorMempoolRpc(ExecutorMempoolRpc const &) = delete;
  ExecutorMempoolRpc &operator=(ExecutorMempoolRpc const &) = delete;
  ExecutorMempoolRpc(ExecutorMempoolRpc &&) = delete;
  ExecutorMempoolRpc &operator=(ExecutorMempoolRpc &&) = delete;

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
  // Does not block.  State will not be garbage collected until all
  // transactions on this block have been committed or aborted.
  void executorcoord_finish_block(BlockID id);

  // It is an error to shut down the executor while there are transactions
  // that are still running.
  x23::Async<void> executorcoord_shutdown();

  void shut_down();
  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  TransactionID allocate_tx_id();

  std::atomic<uint64_t> self_id_ = 0;
  uint16_t self_executor_id_ = std::numeric_limits<uint16_t>::max();

  std::unique_ptr<ExecutorStateManager> executor_states_;
  std::atomic<bool> shutdown_flag_ = false;
};

static_assert(is_ExecutorSched1<ExecutorMempoolRpc *, x23::MaybeAsync>);

} // namespace hogd
