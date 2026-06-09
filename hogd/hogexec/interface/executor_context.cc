#include "hogd/hogexec/interface/executor_context.h"

#include <atomic>
#include <cassert>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

#include "exec_result.hh"
#include "exec_sched.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/view/view.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "view.hh"
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
ExecutorContext::propose_transaction(Transaction const &tx, TransactionID tx_id)
{
  assert(mode_ == ExecutionMode::kProposing);
  x23::Awaitable<ExecProposeResult> awaitable;
  x23::CoroCB<ExecProposeResult> cb(awaitable);
  worker_pool_.submit([this, &tx, tx_id, cb]() {
    auto res = run_propose_transaction(tx, tx_id);
    cb(res);
  });
  co_return co_await awaitable;
}

x23::Async<ExecProposeBatchResult>
ExecutorContext::propose_transaction_batch(TransactionBatch const &txs,
                                           TransactionID first_tx_id)
{
  assert(mode_ == ExecutionMode::kProposing);
  x23::Awaitable<ExecProposeBatchResult> awaitable;
  x23::CoroCB<ExecProposeBatchResult> cb(awaitable);
  worker_pool_.submit([this, &txs, first_tx_id, cb]() {
    ExecProposeBatchResult batch_res;
    batch_res.reserve(txs.size());
    for (size_t i = 0; i < txs.size(); i++) {
      batch_res.push_back(run_propose_transaction(txs[i], first_tx_id + i));
    }
    cb(std::move(batch_res));
  });
  auto r = co_await awaitable;
  assert(r.size() == txs.size());
  co_return std::move(r);
}

x23::Async<ExpectedViewResult>
ExecutorContext::view_transaction(TransactionViewQuery const &query)
{
  assert(mode_ == ExecutionMode::kViewing);
  x23::Awaitable<ExpectedViewResult> awaitable;
  x23::CoroCB<ExpectedViewResult> cb(awaitable);
  worker_pool_.submit([this, &query, cb]() {
    auto res = run_view_transaction(query);
    cb(res);
  });
  auto r = co_await awaitable;
  co_return std::move(r);
}

x23::Async<ExecValidateBatchResult>
ExecutorContext::validate_transaction_batch(
    ProposedTransactionBatch const &proposed_txs, TransactionID first_tx_id)
{
  assert(mode_ == ExecutionMode::kValidating);
  x23::Awaitable<ExecValidateBatchResult> awaitable;
  x23::CoroCB<ExecValidateBatchResult> cb(awaitable);
  worker_pool_.submit([this, &proposed_txs, first_tx_id, cb]() {
    ExecValidateBatchResult batch_res;
    batch_res.reserve(proposed_txs.size());
    for (size_t i = 0; i < proposed_txs.size(); i++) {
      batch_res.push_back(
          run_validate_transaction(proposed_txs[i], first_tx_id + i));
    }
    cb(std::move(batch_res));
  });
  auto r = co_await awaitable;
  assert(r.size() == proposed_txs.size());
  co_return std::move(r);
}

x23::Async<ExecResult>
ExecutorContext::validate_transaction(ProposedTransaction const &proposed_tx,
                                      TransactionID tx_id)
{
  assert(mode_ == ExecutionMode::kValidating);

  x23::Awaitable<ExecResult> awaitable;
  x23::CoroCB<ExecResult> cb(awaitable);

  worker_pool_.submit([this, &proposed_tx, tx_id, cb]() {
    auto res = run_validate_transaction(proposed_tx, tx_id);
    cb(res);
  });

  co_return co_await awaitable;
}

// Stale-block execution during propose/validate is benign.
//
// The StorageSnapshotCache can mask HogDB stale-block detection: if
// deployment metadata, contract code, or storage items are already cached,
// later reads return the cached value without hitting HogDB and the
// Abandon error code is never surfaced.  This is fine here because:
//
//  - A proposal that executes against a stale block produces a candidate
//    that will never be committed (the coordinator commits at most one
//    sibling, and the stale block's sibling already won).
//  - Validation replays the same proposal data on the same block, so
//    staleness is symmetric.
//  - Stale proposal blocks are cleaned up lazily when the block is closed
//    or the next block opens.
//
// The view gateway, by contrast, does care about staleness — it serves
// public RPCs and must not return arbitrarily old data.  See
// ViewGatewayRpc::request_refresh() for the view-side mitigation.
ExecProposeResult
ExecutorContext::run_propose_transaction(Transaction const &tx,
                                         TransactionID tx_id)
{
  static thread_local auto context = make_hogexec_context();
  assert(mode_ == ExecutionMode::kProposing);

  auto proxy = make_per_tx_db_proxy(tx_id);
  SystemCallHandler syscall_handler(tx, *snapshot_cache_, proxy,
                                    make_per_tx_event_server_proxy(tx_id),
                                    ProposalModeCtor{}, *context);
  syscall_handler.root_invoke();
  auto exec_res = syscall_handler.try_commit_transaction();

  auto out = ExecProposeResult(exec_res);

  switch (exec_res) {
  case ExecResult::ExecResultOK:
  case ExecResult::ExecResultTxDeterministicError:
  case ExecResult::ExecResultTxRootFrameReverted:
    out.proposal_data() = syscall_handler.get_proposal_data();
    break;
  case ExecResult::ExecResultTxNonDeterministicError:
  case ExecResult::ExecResultFailedGasSponsorship:
  case ExecResult::ExecResultTxValidationFailure:
  case ExecResult::ExecResultSystemError:
  case ExecResult::ExecResultUnrecoverable:
    // nothing to do: a rejected transaction (e.g. an expired uniquifier, #2568,
    // which surfaces as a ValidationFailure) is not included in the proposal.
    break;
  }

  if (exec_res == ExecResult::ExecResultOK ||
      exec_res == ExecResult::ExecResultTxDeterministicError ||
      exec_res == ExecResult::ExecResultTxRootFrameReverted) {
    // Use release ordering to ensure the event RPC spawn is visible to
    // other threads before they see the incremented counter.
    pending_event_calls_.fetch_add(1, std::memory_order_release);
    // The release of the PerThreaadARC for this happens after the above
    // fetch_add; hence, when finish_block() is called, the above change is
    // visible.
    //
    // Furthermore, it is not possible that the acquire_reference() for this
    // transaction could succeed if the PerThreadARC is in shutdown mode. (If
    // someone sends a bunch of propose_transaction() calls and then immediately
    // calls finish_block() without waiting for those, and uses different tcp
    // connections so the calls aren't ordered, then some of those propose calls
    // might still fail by failing to acquire a reference).
    //
    // Hence, once shutdown has started, the final cleanup owner can drain
    // pending_event_calls_ before this ExecutorContext is destroyed.
    syscall_handler.emit_events(exec_res)(
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
  static thread_local auto context = make_hogexec_context();
  assert(mode_ == ExecutionMode::kValidating);
  auto proxy = make_per_tx_db_proxy(tx_id);
  SystemCallHandler syscall_handler(tx.signed_tx.tx, *snapshot_cache_, proxy,
                                    make_per_tx_event_server_proxy(tx_id),
                                    ValidationModeCtor{
                                        tx.nondeterministic_proposal_data},
                                    *context);
  syscall_handler.root_invoke();
  auto exec_res = syscall_handler.try_commit_transaction();

  if (exec_res == ExecResult::ExecResultOK ||
      exec_res == ExecResult::ExecResultTxDeterministicError ||
      exec_res == ExecResult::ExecResultTxRootFrameReverted) {
    // Use release ordering to ensure the event RPC spawn is visible to
    // other threads before they see the incremented counter.
    pending_event_calls_.fetch_add(1, std::memory_order_release);
    syscall_handler.emit_events(exec_res)(
        [this](SystemStatus<void> res) -> x23::Detached {
          log_event_return(res);
          co_return void();
        });
  }
  return exec_res;
}

ExpectedViewResult
ExecutorContext::run_view_transaction(TransactionViewQuery const &query)
{
  assert(mode_ == ExecutionMode::kViewing);
  static thread_local auto context = make_hogexec_context();
  return View::run_view_transaction(
      *snapshot_cache_,
      make_per_tx_db_proxy(std::numeric_limits<TransactionID>::max()),
      make_per_tx_event_server_proxy(std::numeric_limits<TransactionID>::max()),
      query, *context, {});
}

void
ExecutorContext::log_event_return(SystemStatus<void> err)
{
  if (!err.has_value()) {
    std::unique_lock lock(event_accumulate_mutex_);
    event_error_ = err.error().code();
    error("event error: {}", std::to_underlying(event_error_.value()));
  }
  // Use release ordering to ensure all event RPC operations (including
  // server-side add_event/write_event) are visible before the decrement.
  pending_event_calls_.fetch_sub(1, std::memory_order_release);
}

x23::Async<std::optional<SystemErrorCode>>
ExecutorContext::await_event_return()
{
  uint32_t wait_count = 0;
  // Use acquire ordering to ensure we see all operations that happened
  // before the release-store in log_event_return, including all server-side
  // event operations.
  while (pending_event_calls_.load(std::memory_order_acquire) > 0) {
    cpu_relax();
    wait_count++;
    if ((wait_count & 0xFF) == 0U) [[unlikely]] {
      co_await default_threadpool().bg();
    }
  }
  co_return event_error_;
}

} // namespace hogd
