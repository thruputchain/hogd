#include "hogd/hogexec/interface/executor_context.h"

#include <cassert>

#include "exec_sched.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"

namespace hogd {

PerTransactionDatabaseProxy
ExecutorContext::make_per_tx_db_proxy(TransactionID tx_id)
{
  return PerTransactionDatabaseProxy(db_proxy_, block_id_, tx_id);
}

PerTransactionEventServerProxy
ExecutorContext::make_per_tx_event_server_proxy(TransactionID tx_id)
{
  return PerTransactionEventServerProxy(event_server_proxy_, block_id_, tx_id);
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
  auto [async, cb] = x23::make_async<ExecProposeResult>();
  worker_pool_.submit([this, &signed_tx, tx_id, cb]() {
    auto res = run_propose_transaction(signed_tx, tx_id);
    cb(res);
  });
  co_return co_await async;
}

x23::Async<ExecProposeBatchResult>
ExecutorContext::propose_transaction_batch(
    SignedTransactionBatch const &signed_txs, TransactionID first_tx_id)
{
  assert(mode_ == ExecutionMode::kProposing);
  auto [async, cb] = x23::make_async<ExecProposeBatchResult>();
  worker_pool_.submit([this, &signed_txs, first_tx_id, cb]() {
    ExecProposeBatchResult batch_res;
    for (size_t i = 0; i < signed_txs.size(); i++) {
      auto res = run_propose_transaction(signed_txs[i], first_tx_id + i);
      batch_res.push_back(res);
    }
    cb(batch_res);
  });
  co_return co_await async;
}

x23::Async<ExecValidateBatchResult>
ExecutorContext::validate_transaction_batch(
    ProposedTransactionBatch const &proposed_txs, TransactionID first_tx_id)
{
  assert(mode_ == ExecutionMode::kValidating);
  auto [async, cb] = x23::make_async<ExecValidateBatchResult>();
  worker_pool_.submit([this, &proposed_txs, first_tx_id, cb]() {
    ExecValidateBatchResult batch_res;
    for (size_t i = 0; i < proposed_txs.size(); i++) {
      auto res = run_validate_transaction(proposed_txs[i], first_tx_id + i);
      batch_res.push_back(res);
    }
    cb(batch_res);
  });
  co_return co_await async;
}

x23::Async<ExecResult>
ExecutorContext::validate_transaction(ProposedTransaction const &proposed_tx,
                                      TransactionID tx_id)
{
  assert(mode_ == ExecutionMode::kValidating);

  auto [async, cb] = x23::make_async<ExecResult>();

  worker_pool_.submit([this, &proposed_tx, tx_id, cb]() {
    auto res = run_validate_transaction(proposed_tx, tx_id);
    cb(res);
  });

  co_return co_await async;
}

ExecProposeResult
ExecutorContext::run_propose_transaction(SignedTransaction const &tx,
                                         TransactionID tx_id)
{
  assert(mode_ == ExecutionMode::kProposing);
  auto proxy = make_per_tx_db_proxy(tx_id);
  SystemCallHandler syscall_handler(tx, snapshot_cache_, proxy,
                                    make_per_tx_event_server_proxy(tx_id),
                                    ProposalModeCtor{}, make_hogexec_context());
  auto invoke_res = syscall_handler.root_invoke();

  if (!invoke_res.is_root_success()) {
    auto st = proxy.abort_transaction();
    if (!st.has_value()) {
      return ExecProposeResult(make_exec_result(st.error().code()));
    }
    assert(!invoke_res.result_.has_value());

    auto out =
        ExecProposeResult(make_exec_result(invoke_res.result_.error().code()));
    assert(invoke_res.result_.error().code() != InvokeErrorCode::kInvokeNone);
    if (invoke_res.result_.error().code() ==
        InvokeErrorCode::kInvokeDeterministic) {
      out.proposal_data() = syscall_handler.get_proposal_data();
    }
    return out;
  }

  auto commit_res = syscall_handler.try_commit_transaction();
  if (!commit_res.has_value()) {
    // this error code should not be returned by the try_commit_transaction
    // function
    assert(commit_res.error().code() != SystemErrorCode::kValidationFailure);
    return ExecProposeResult(make_exec_result(commit_res.error().code()));
  }

  if (commit_res.value()) {
    auto out = ExecProposeResult(ExecResult::ExecResultOK);
    out.proposal_data() = syscall_handler.get_proposal_data();
    return out;
  }
  return ExecProposeResult(ExecResult::ExecResultTxNonDeterministicError);
}

ExecResult
ExecutorContext::run_validate_transaction(ProposedTransaction const &tx,
                                          TransactionID tx_id)
{
  assert(mode_ == ExecutionMode::kValidating);
  auto proxy = make_per_tx_db_proxy(tx_id);
  SystemCallHandler syscall_handler(tx.signed_tx, snapshot_cache_, proxy,
                                    make_per_tx_event_server_proxy(tx_id),
                                    ValidationModeCtor{
                                        tx.nondeterministic_proposal_data},
                                    make_hogexec_context());
  auto invoke_res = syscall_handler.root_invoke();

  if (!invoke_res.is_root_success()) {
    auto res = proxy.abort_transaction();
    if (!res.has_value()) {
      return make_exec_result(res.error().code());
    }
    assert(!invoke_res.result_.has_value());
    return make_exec_result(invoke_res.result_.error().code());
  }

  auto commit_res = syscall_handler.try_commit_transaction();
  if (!commit_res.has_value()) {
    // this error code should not be returned by the try_commit_transaction
    // function
    assert(commit_res.error().code() != SystemErrorCode::kValidationFailure);
    return make_exec_result(commit_res.error().code());
  }

  if (commit_res.value()) {
    return ExecResult::ExecResultOK;
  }
  return ExecResult::ExecResultTxNonDeterministicError;
}

} // namespace hogd
