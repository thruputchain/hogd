#include "hogd/hogexec/interface/executor_context.h"

#include <atomic>
#include <cassert>
#include <mutex>
#include <optional>
#include <utility>

#include "exec_result.hh"
#include "exec_sched.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
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

x23::Async<ExecProposeResult>
ExecutorContext::propose_transaction(SignedTransaction const &signed_tx,
                                     TransactionID tx_id)
{
  assert(mode_ == ExecutionMode::kProposing);
  auto async_pair = x23::make_async<ExecProposeResult>();
  x23::Async<ExecProposeResult> &async = async_pair.first;
  x23::CoroCB<ExecProposeResult> &cb = async_pair.second;
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
  auto async_pair = x23::make_async<ExecProposeBatchResult>();
  x23::Async<ExecProposeBatchResult> &async = async_pair.first;
  x23::CoroCB<ExecProposeBatchResult> &cb = async_pair.second;
  worker_pool_.submit([this, &signed_txs, first_tx_id, cb]() {
    ExecProposeBatchResult batch_res;
    for (size_t i = 0; i < signed_txs.size(); i++) {
      batch_res.push_back(
          run_propose_transaction(signed_txs[i], first_tx_id + i));
    }
    cb(std::move(batch_res));
  });
  auto r = co_await async;
  assert(r.size() == signed_txs.size());
  co_return r;
}

x23::Async<ExecValidateBatchResult>
ExecutorContext::validate_transaction_batch(
    ProposedTransactionBatch const &proposed_txs, TransactionID first_tx_id)
{
  assert(mode_ == ExecutionMode::kValidating);
  auto async_pair = x23::make_async<ExecValidateBatchResult>();
  x23::Async<ExecValidateBatchResult> &async = async_pair.first;
  x23::CoroCB<ExecValidateBatchResult> &cb = async_pair.second;
  worker_pool_.submit([this, &proposed_txs, first_tx_id, cb]() {
    ExecValidateBatchResult batch_res;

    for (size_t i = 0; i < proposed_txs.size(); i++) {
      batch_res.push_back(
          run_validate_transaction(proposed_txs[i], first_tx_id + i));
    }
    cb(std::move(batch_res));
  });
  co_return co_await async;
}

x23::Async<ExecResult>
ExecutorContext::validate_transaction(ProposedTransaction const &proposed_tx,
                                      TransactionID tx_id)
{
  assert(mode_ == ExecutionMode::kValidating);

  auto async_pair = x23::make_async<ExecResult>();
  x23::Async<ExecResult> &async = async_pair.first;
  x23::CoroCB<ExecResult> &cb = async_pair.second;

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
  SystemCallHandler syscall_handler(tx, *snapshot_cache_, proxy,
                                    make_per_tx_event_server_proxy(tx_id),
                                    ProposalModeCtor{}, make_hogexec_context());
  syscall_handler.root_invoke();
  auto exec_res = syscall_handler.try_commit_transaction();

  auto out = ExecProposeResult(exec_res);

  switch (exec_res) {
  case ExecResult::ExecResultOK:
  case ExecResult::ExecResultTxDeterministicError:
    out.proposal_data() = syscall_handler.get_proposal_data();
    break;
  case ExecResult::ExecResultTxNonDeterministicError:
  case ExecResult::ExecResultFailedGasSponsorship:
  case ExecResult::ExecResultTxValidationFailure:
  case ExecResult::ExecResultSystemError:
  case ExecResult::ExecResultUnrecoverable:
    // nothing to do
    break;
  }

  if (exec_res == ExecResult::ExecResultOK) {
    pending_event_calls_.fetch_add(1, std::memory_order_relaxed);
    syscall_handler.emit_events()(
        [this](SystemStatus<void> res) -> x23::Detached {
          log_event_return(res);
          co_return;
        });
  }

  return out;
}

ExecResult
ExecutorContext::run_validate_transaction(ProposedTransaction const &tx,
                                          TransactionID tx_id)
{
  assert(mode_ == ExecutionMode::kValidating);
  auto proxy = make_per_tx_db_proxy(tx_id);
  SystemCallHandler syscall_handler(tx.signed_tx, *snapshot_cache_, proxy,
                                    make_per_tx_event_server_proxy(tx_id),
                                    ValidationModeCtor{
                                        tx.nondeterministic_proposal_data},
                                    make_hogexec_context());
  syscall_handler.root_invoke();
  auto exec_res = syscall_handler.try_commit_transaction();

  if (exec_res == ExecResult::ExecResultOK) {
    pending_event_calls_.fetch_add(1, std::memory_order_relaxed);
    syscall_handler.emit_events()(
        [this](SystemStatus<void> res) -> x23::Detached {
          log_event_return(res);
          co_return void();
        });
  }
  return exec_res;
}

void
ExecutorContext::log_event_return(SystemStatus<void> err)
{
  if (!err.has_value()) {
    std::unique_lock lock(event_accumulate_mutex_);
    event_error_ = err.error().code();
    error("event error: {}", std::to_underlying(event_error_.value()));
  }
  pending_event_calls_.fetch_sub(1, std::memory_order_relaxed);
}

std::optional<SystemErrorCode>
ExecutorContext::await_event_return()
{
  while (pending_event_calls_.load(std::memory_order_relaxed) > 0) {
    cpu_relax();
  }
  return event_error_;
}

} // namespace hogd
