#include "hogd/hogexec/interface/rpc_interface.h"

#include <atomic>
#include <cassert>

#include "exec_coord.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"

namespace hogd {

x23::Async<ExecProposeResult>
ExecutorRpcInterface::executorcoord_propose_transaction(
    BlockID block_id, SignedTransaction const &signed_tx)
{
  auto ctx = executor_states_.get_executor_context(block_id,
                                                   ExecutionMode::kProposing);
  if (!ctx) {
    co_return ExecProposeResult(ExecResult::ExecResultSystemError);
  }
  // at this point, we have a run lease for ctx, so
  // ctx is guaranteed to be a valid pointer until release_run_lease() is called

  PerTransactionDatabaseProxy proxy(ctx->db_proxy_, block_id, allocate_tx_id());

  SystemCallHandler syscall_handler(signed_tx, ctx->snapshot_cache_, proxy,
                                    ProposalModeData{});

  auto [async, cb] = x23::make_async<InvokeResult>();

  main_worker_pool_.submit([&syscall_handler, &cb] -> void {
    auto res = syscall_handler.root_invoke();
    cb(res);
  });

  auto invoke_res = co_await async;

  if (!invoke_res.is_root_success()) {
    co_await proxy.abort_transaction();
    co_return ExecProposeResult(ExecResult::ExecResultSystemError);
  }

  auto commit_res = co_await syscall_handler.try_commit_transaction();

  if (commit_res) {
    auto out = ExecProposeResult(ExecResult::ExecResultOK);
    out.proposal_data() = syscall_handler.get_proposal_data().proposal_data();
    co_return out;
  }
  co_return ExecProposeResult(ExecResult::ExecResultSystemError);
}

x23::Async<ExecResult>
ExecutorRpcInterface::executorcoord_validate_transaction(
    BlockID block_id, ProposedTransaction const &proposed_tx)
{
  auto ctx = executor_states_.get_executor_context(block_id,
                                                   ExecutionMode::kValidating);
  if (!ctx) {
    co_return ExecResult::ExecResultSystemError;
  }

  PerTransactionDatabaseProxy proxy(ctx->db_proxy_, block_id, allocate_tx_id());

  SystemCallHandler syscall_handler(
      proposed_tx.signed_tx, ctx->snapshot_cache_, proxy,
      ValidationModeData{proposed_tx.nondeterministic_proposal_data});

  auto [async, cb] = x23::make_async<InvokeResult>();

  main_worker_pool_.submit([&syscall_handler, &cb] -> void {
    auto res = syscall_handler.root_invoke();
    cb(res);
  });

  auto invoke_res = co_await async;

  if (!invoke_res.is_root_success()) {
    co_await proxy.abort_transaction();
    co_return ExecResult::ExecResultTxError;
  }

  auto commit_res = co_await syscall_handler.try_commit_transaction();

  if (commit_res) {
    co_return ExecResult::ExecResultOK;
  }
  co_return ExecResult::ExecResultTxError;
}

x23::Async<ExecResult>
ExecutorRpcInterface::executorcoord_open_proposal_block(BlockID id)
{
  executor_states_.open_proposal(id);
  co_return ExecResult::ExecResultOK;
}

x23::Async<ExecResult>
ExecutorRpcInterface::executorcoord_open_validation_block(BlockID id)
{
  executor_states_.open_validation(id);
  co_return ExecResult::ExecResultOK;
}

x23::Async<void>
ExecutorRpcInterface::executorcoord_finish_block(BlockID id)
{
  executor_states_.remove_block(id);
  co_return;
}

TransactionID
ExecutorRpcInterface::allocate_tx_id()
{
  uint64_t base = self_id_.fetch_add(1, std::memory_order_relaxed);
  return TransactionID{base + (uint64_t{self_executor_id_} << 48)};
}

x23::Async<void>
ExecutorRpcInterface::executorcoord_shutdown()
{
  shutdown_flag_ = true;
  co_return;
}

} // namespace hogd
