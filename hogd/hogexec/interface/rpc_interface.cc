#include "hogd/hogexec/interface/rpc_interface.h"

#include "hogd/hogexec/runtime/syscall_handler.h"
#include "rpc.hh"
#include "rpc_types.hh"
#include "x23/cleanup.h"

namespace hogd {

x23::Async<ExpectedProposalData>
ExecutorRpcInterface::propose_transaction(BlockID block_id,
                                          SignedTransaction const &signed_tx)
{
  std::printf("start propose_transaction\n");
  auto *ctx = executor_states_.get_executor_context(block_id);
  if (!ctx) {
    co_return ExpectedProposalData(RPCError::kBadExecutorState);
  }

  if (ctx->state_.try_acquire_run_lease(ExecutorMode::kProposing)) {
    x23::Defer d([&] { ctx->state_.release_run_lease(); });
    // TODO(geoff): merge in proposal data handling code

    auto *pool = x23::ThreadPool::get_thread_pool();
    assert(pool);

    SystemCallHandler syscall_handler(signed_tx, ctx->snapshot_cache_);
    auto invoke_res = co_await pool->async(
        [](SystemCallHandler *handler) -> InvokeResult {
          return handler->root_invoke();
        },
        &syscall_handler);

    if (!invoke_res.is_root_success()) {
      co_return ExpectedProposalData(RPCError::kTransactionFailed);
    }

    PerTransactionDatabaseProxy proxy(block_id, allocate_tx_id(), db_proxy_);

    auto commit_res = co_await syscall_handler.try_commit_transaction(&proxy);

    if (commit_res) {
      co_return ExpectedProposalData(RPCError::kNone);
    }
    co_return ExpectedProposalData(RPCError::kTransactionFailed);
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
  /*
    if (ctx->state_.try_acquire_run_lease(ExecutorMode::kValidating)) {
      x23::Defer d([&] { ctx->state_.release_run_lease(); });

      // TODO(geoff): merge in proposal data handling code
      SystemCallHandler syscall_handler(proposed_tx.signed_tx,
                                        ctx->snapshot_cache_);

      auto invoke_res = syscall_handler.root_invoke();

      if (!invoke_res.is_root_success()) {
        co_return RPCError::kTransactionFailed;
      }

      auto commit_res = co_await syscall_handler.try_commit_transaction(
          PerTransactionDatabaseProxy(block_id, allocate_tx_id(), db_proxy_));

      if (commit_res) {
        co_return RPCError::kNone;
      }
      co_return RPCError::kTransactionFailed;
    } */
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

TransactionID
ExecutorRpcInterface::allocate_tx_id()
{
  uint64_t base = self_id_.fetch_add(1, std::memory_order_relaxed);
  return TransactionID{base + (uint64_t{self_executor_id_} << 48)};
}

} // namespace hogd
