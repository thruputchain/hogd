#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <limits>
#include <memory>
#include <utility>

#include "exec_result.hh"
#include "exec_sched.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/interface/executor_context.h"
#include "hogd/hogexec/interface/executor_state_manager.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
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

  void init(IntroExecResult &&exec_result, size_t n_exec_threads)
  {
    if (exec_result.exec_id >= std::numeric_limits<uint16_t>::max())
        [[unlikely]] {
      panic("ExecutorMempoolRpc: exec_id is too large");
      return;
    }
    self_executor_id_ = exec_result.exec_id;

    DatabaseIdentifier identifier{ShardedDB(exec_result)};
    auto db_proxy = std::make_unique<DatabaseProxy>(std::move(identifier));
    auto event_server_proxy = std::make_unique<EventServerProxy>(
        EventServerProxy::make_from_exec_result(exec_result.event));
    executor_states_ =
        std::make_unique<ExecutorStateManager>(std::move(db_proxy),
                                               std::move(event_server_proxy),
                                               n_exec_threads);
    info("ExecutorMempoolRpc: initialized with {} threads", n_exec_threads);
    ready_flag_ = true;
  }

  ExecResult executorsched_ready()
  {
    if (!ready_flag_)
      return ExecResult::ExecResultSystemError;
    return ExecResult::ExecResultOK;
  }

  // Disable copy and move
  ExecutorMempoolRpc(ExecutorMempoolRpc const &) = delete;
  ExecutorMempoolRpc &operator=(ExecutorMempoolRpc const &) = delete;
  ExecutorMempoolRpc(ExecutorMempoolRpc &&) = delete;
  ExecutorMempoolRpc &operator=(ExecutorMempoolRpc &&) = delete;

  static x23::Async<void> executorsched_null() { co_return; }

  x23::Async<ExecProposeResult>
  executorsched_propose_transaction(BlockID block_id, SignedTransaction signed_tx);
  x23::Async<ExecResult>
  executorsched_validate_transaction(BlockID block_id,
                                     ProposedTransaction proposed_tx);

  x23::Async<ExecProposeBatchResult>
  executorsched_propose_batch(BlockID block_id, SignedTransactionBatch signed_txs);
  x23::Async<ExecValidateBatchResult>
  executorsched_validate_batch(BlockID block_id,
                               ProposedTransactionBatch proposed_txs);

  x23::Async<ExecResult> executorsched_open_proposal_block(BlockID id);
  x23::Async<ExecResult> executorsched_open_validation_block(BlockID id);
  // Indicates that state associated with this block can be garbage collected.
  // Does not block.  State will not be garbage collected until all
  // transactions on this block have been committed or aborted.
  x23::Async<ExecResult> executorsched_finish_block(BlockID id);

  // It is an error to shut down the executor while there are transactions
  // that are still running.
  x23::Async<void> executorsched_shutdown();

  void shut_down()
  {
    shutdown_flag_ = true;
    shutdown_flag_.notify_all();
  }

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  // Allocates a contiguous block of transaction IDs.
  // Returns the first transaction ID in the block
  // (so i.e. the allocated ids are [first_id, first_id + count - 1] inclusive)
  TransactionID allocate_tx_ids(uint32_t count);

  ATTRIBUTE_ALWAYS_INLINE bool initialize_check()
  {
    return executor_states_ != nullptr;
  }

  // Acquires a reference to the context for block_id in the given mode. A null
  // value means there is no usable context (not initialized, not found, or a
  // mode mismatch); an error means a late event-server error surfaced while
  // releasing during a mode-mismatch cleanup and must be propagated.
  x23::Async<SystemStatus<PerThreadARC<ExecutorContext> *>>
  try_get_context(BlockID block_id, ExecutionMode mode);
  // Releases the caller's reference. If this was the last reference, runs the
  // deferred gc and returns its result so a late event-server error can be
  // folded into the in-flight transaction's result.
  x23::MaybeAsync<SystemStatus<void>>
  release_context(BlockID block_id, PerThreadARC<ExecutorContext> *ctx);

  struct TxIDAllocator {
    uint64_t value_ = 0;
    uint64_t limit_ = 0;
    std::atomic<uint64_t> *base_pool_ = nullptr;

    uint64_t allocate(uint32_t count)
    {
      assert(base_pool_ != nullptr);
      if (value_ + count > limit_) {
        value_ =
            base_pool_->fetch_add(std::max<uint64_t>(kBaseAllocSize, count),
                                  std::memory_order_relaxed);
        limit_ = value_ + std::max<uint64_t>(kBaseAllocSize, count);
      }
      uint64_t result = value_;
      value_ += count;
      return result;
    }
  };

  PerThread<TxIDAllocator> tx_id_allocator_;
  std::atomic<uint64_t> base_alloc_pool_ = 0;
  constexpr static uint64_t kBaseAllocSize = 256;

  uint16_t self_executor_id_ = std::numeric_limits<uint16_t>::max();

  std::unique_ptr<ExecutorStateManager> executor_states_;
  std::atomic<bool> shutdown_flag_ = false;
  std::atomic<bool> ready_flag_ = false;
};

static_assert(is_ExecutorSched1<ExecutorMempoolRpc *, x23::MaybeAsync>);

} // namespace hogd
