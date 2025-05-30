#include "hogd/hogexec/interface/executor_context.h"

#include "exec_coord.hh"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"
#include <cassert>

namespace hogd {

PerTransactionDatabaseProxy
ExecutorContext::make_per_tx_db_proxy(TransactionID tx_id)
{
  return PerTransactionDatabaseProxy(db_proxy_, block_id_, tx_id);
}

x23::Async<ExecProposeResult>
ExecutorContext::propose_transaction(SignedTransaction const &signed_tx,
                                     TransactionID tx_id)
{
  assert(mode_ == ExecutionMode::kProposing);
  auto proxy = make_per_tx_db_proxy(tx_id);
  SystemCallHandler syscall_handler(signed_tx, snapshot_cache_, proxy,
                                    ProposalModeData{});

  auto [async, cb] = x23::make_async<InvokeResult>();

  main_worker_pool_.submit([&syscall_handler, &cb] -> void {
    auto res = syscall_handler.root_invoke();
    cb(res);
  });

  auto invoke_res = co_await async;

  if (!invoke_res.is_root_success()) {
    co_await proxy.abort_transaction();
    co_return ExecProposeResult(ExecResult::ExecResultTxError);
  }

  auto commit_res = co_await syscall_handler.try_commit_transaction();
  if (!commit_res.has_value()) {
    co_return ExecProposeResult(ExecResult::ExecResultSystemError);
  }

  if (commit_res.value()) {
    auto out = ExecProposeResult(ExecResult::ExecResultOK);
    out.proposal_data() = syscall_handler.get_proposal_data().proposal_data();
    co_return out;
  }
  co_return ExecProposeResult(ExecResult::ExecResultTxError);
}

x23::Async<ExecResult>
ExecutorContext::validate_transaction(ProposedTransaction const &proposed_tx,
                                      TransactionID tx_id)
{
  assert(mode_ == ExecutionMode::kValidating);
  auto proxy = make_per_tx_db_proxy(tx_id);

  SystemCallHandler syscall_handler(
      proposed_tx.signed_tx, snapshot_cache_, proxy,
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
  if (!commit_res.has_value()) {
    co_return ExecResult::ExecResultSystemError;
  }

  if (commit_res.value()) {
    co_return ExecResult::ExecResultOK;
  }
  co_return ExecResult::ExecResultTxError;
}

} // namespace hogd
