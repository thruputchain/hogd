#include "hogd/hogexec/interface/executor_context.h"

#include "exec_coord.hh"
#include "ffi_enums.hh"
#include "hogd/hogexec/precompiles/context.h"
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

namespace {
ExecResult
make_exec_result(InvokeErrorCode error_code)
{
  switch (error_code) {
  case InvokeErrorCode::kInvokeNone:
    return ExecResult::ExecResultOK;
  case InvokeErrorCode::kInvokeAbortTransaction:
    return ExecResult::ExecResultSystemError;
  case InvokeErrorCode::kInvokeDeterministic:
  case InvokeErrorCode::kInvokeOutOfGas:
    return ExecResult::ExecResultTxDeterministicError;
  case InvokeErrorCode::kInvokeValidationFailure:
    return ExecResult::ExecResultTxValidationFailure;
  case InvokeErrorCode::kInvokeUnrecoverable:
    return ExecResult::ExecResultUnrecoverable;
  }
  assert(false);
  return ExecResult::ExecResultUnrecoverable;
}

ExecResult
make_exec_result(SystemErrorCode error_code)
{
  switch (error_code) {
  case SystemErrorCode::kUnrecoverable:
    return ExecResult::ExecResultUnrecoverable;
  case SystemErrorCode::kAbortTransaction:
    return ExecResult::ExecResultSystemError;
  case SystemErrorCode::kValidationFailure:
    return ExecResult::ExecResultTxValidationFailure;
  }
  assert(false);
  return ExecResult::ExecResultUnrecoverable;
}
} // namespace

x23::Async<ExecProposeResult>
ExecutorContext::propose_transaction(SignedTransaction const &signed_tx,
                                     TransactionID tx_id)
{
  assert(mode_ == ExecutionMode::kProposing);
  auto proxy = make_per_tx_db_proxy(tx_id);
  SystemCallHandler syscall_handler(signed_tx, snapshot_cache_, proxy,
                                    ProposalModeData{}, make_hogexec_context());

  auto invoke_res = syscall_handler.root_invoke();

  if (!invoke_res.is_root_success()) {
    auto st = proxy.abort_transaction();
    if (!st.has_value()) {
      co_return ExecProposeResult(make_exec_result(st.error().code()));
    }
    assert(!invoke_res.result_.has_value());

    auto out =
        ExecProposeResult(make_exec_result(invoke_res.result_.error().code()));
    assert(invoke_res.result_.error().code() != InvokeErrorCode::kInvokeNone);
    if (invoke_res.result_.error().code() ==
        InvokeErrorCode::kInvokeDeterministic) {
      out.proposal_data() = syscall_handler.get_proposal_data().proposal_data();
    }
    co_return out;
  }

  auto commit_res = syscall_handler.try_commit_transaction();
  if (!commit_res.has_value()) {
    // this error code should not be returned by the try_commit_transaction
    // function
    assert(commit_res.error().code() != SystemErrorCode::kValidationFailure);
    co_return ExecProposeResult(make_exec_result(commit_res.error().code()));
  }

  if (commit_res.value()) {
    auto out = ExecProposeResult(ExecResult::ExecResultOK);
    out.proposal_data() = syscall_handler.get_proposal_data().proposal_data();
    co_return out;
  }
  co_return ExecProposeResult(ExecResult::ExecResultTxNonDeterministicError);
}

x23::Async<ExecResult>
ExecutorContext::validate_transaction(ProposedTransaction const &proposed_tx,
                                      TransactionID tx_id)
{
  assert(mode_ == ExecutionMode::kValidating);
  auto proxy = make_per_tx_db_proxy(tx_id);

  SystemCallHandler syscall_handler(
      proposed_tx.signed_tx, snapshot_cache_, proxy,
      ValidationModeData{proposed_tx.nondeterministic_proposal_data},
      make_hogexec_context());

  auto [async, cb] = x23::make_async<InvokeResult>();

  main_worker_pool_.submit([&syscall_handler, &cb] -> void {
    auto res = syscall_handler.root_invoke();
    cb(res);
  });

  auto invoke_res = co_await async;

  if (!invoke_res.is_root_success()) {
    auto res = proxy.abort_transaction();
    if (!res.has_value()) {
      co_return make_exec_result(res.error().code());
    }
    assert(!invoke_res.result_.has_value());
    co_return make_exec_result(invoke_res.result_.error().code());
  }

  auto commit_res = syscall_handler.try_commit_transaction();
  if (!commit_res.has_value()) {
    // this error code should not be returned by the try_commit_transaction
    // function
    assert(commit_res.error().code() != SystemErrorCode::kValidationFailure);
    co_return make_exec_result(commit_res.error().code());
  }

  if (commit_res.value()) {
    co_return ExecResult::ExecResultOK;
  }
  co_return ExecResult::ExecResultTxNonDeterministicError;
}

} // namespace hogd
