#pragma once

#include <atomic>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include "exec_sched.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/interface/executor_context.h"
#include "hogd/hogexec/interface/executor_state_manager.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "rpc_types.hh"
#include "sched_exec.hh"
#include "transaction.hh"
#include "x23/async.h"

namespace hogd {

// Calls from coordinator into executor
class ExecutorMempoolRpc {
public:
  using rpc_service = ExecutorSched1;

  ExecutorMempoolRpc() = default;

  void init(IntroExecResult &&exec_result)
  {
    self_executor_id_ = exec_result.exec_id;

    DatabaseIdentifier identifier{ShardedDB(exec_result)};
    auto db_proxy = std::make_unique<DatabaseProxy>(std::move(identifier));
    auto event_server_proxy = std::make_unique<EventServerProxy>(
        EventServerProxy::make_from_exec_result(exec_result.event));
    executor_states_ =
        std::make_unique<ExecutorStateManager>(std::move(db_proxy),
                                               std::move(event_server_proxy));
  }

  // Disable copy and move
  ExecutorMempoolRpc(ExecutorMempoolRpc const &) = delete;
  ExecutorMempoolRpc &operator=(ExecutorMempoolRpc const &) = delete;
  ExecutorMempoolRpc(ExecutorMempoolRpc &&) = delete;
  ExecutorMempoolRpc &operator=(ExecutorMempoolRpc &&) = delete;

  static x23::Async<void> executorsched_null() { co_return; }

  x23::Async<ExecProposeResult>
  executorsched_propose_transaction(BlockID block_id,
                                    SignedTransaction signed_tx);
  x23::Async<ExecResult>
  executorsched_validate_transaction(BlockID block_id,
                                     ProposedTransaction proposed_tx);

  x23::Async<ExecProposeBatchResult>
  executorsched_propose_batch(BlockID block_id,
                              SignedTransactionBatch signed_txs);
  x23::Async<ExecValidateBatchResult>
  executorsched_validate_batch(BlockID block_id,
                               ProposedTransactionBatch proposed_txs);

  ExecResult executorsched_open_proposal_block(BlockID id);
  ExecResult executorsched_open_validation_block(BlockID id);
  // Indicates that state associated with this block can be garbage collected.
  // Does not block.  State will not be garbage collected until all
  // transactions on this block have been committed or aborted.
  void executorsched_finish_block(BlockID id);

  // It is an error to shut down the executor while there are transactions
  // that are still running.
  x23::Async<void> executorsched_shutdown();

  void shut_down()
  {
    // TODO(jhumphri): Implement.
    shutdown_flag_ = true;
  }

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  // Allocates a contiguous block of transaction IDs.
  // Returns the first transaction ID in the block
  // (so i.e. the allocated ids are [first_id, first_id + count - 1] inclusive)
  TransactionID allocate_tx_ids(size_t count);

  ATTRIBUTE_ALWAYS_INLINE bool initialize_check()
  {
    return executor_states_ != nullptr;
  }

  std::shared_ptr<ExecutorContext> try_get_context(BlockID block_id,
                                                   ExecutionMode mode);

  std::atomic<uint64_t> self_id_ = 0;
  uint16_t self_executor_id_ = std::numeric_limits<uint16_t>::max();

  std::unique_ptr<ExecutorStateManager> executor_states_;
  std::atomic<bool> shutdown_flag_ = false;
};

static_assert(is_ExecutorSched1<ExecutorMempoolRpc *, x23::MaybeAsync>);

} // namespace hogd
