#include "hogd/hog_exec/interface/executor_context.h"

#include <atomic>
#include <cassert>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

#include "exec_result.hh"
#include "exec_sched.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/limits.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/execution_mode.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/event_server_proxy.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/view/view.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "view.hh"
#include "x23/async.h"
#include "x23/serialize.h"

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
ExecutorContext::propose_transaction(SignedTransaction signed_tx,
                                     TransactionID tx_id)
{
  assert(mode_ == ExecutionMode::kProposing);
  // Get off the RPC reader before executing.  x23's reader coroutine calls
  // serve() inline, so without this hop the body runs on the reader up to its
  // first database read and every other request on that connection waits
  // behind it -- and a connection serves many transactions at once.  main did
  // not have this problem because it submitted to a DispatchPool and parked on
  // the result, which returned to the reader immediately.
  co_await default_threadpool().bg();
  co_return co_await run_propose_transaction(std::move(signed_tx), tx_id);
}

x23::Async<ExecProposeBatchResult>
ExecutorContext::propose_transaction_batch(SignedTransactionBatch signed_txs,
                                           TransactionID first_tx_id)
{
  assert(mode_ == ExecutionMode::kProposing);
  // Get off the RPC reader before executing.  x23's reader coroutine calls
  // serve() inline, so without this hop the body runs on the reader up to its
  // first database read and every other request on that connection waits
  // behind it -- and a connection serves many transactions at once.  main did
  // not have this problem because it submitted to a DispatchPool and parked on
  // the result, which returned to the reader immediately.
  co_await default_threadpool().bg();
  ExecProposeBatchResult batch_res;
  batch_res.reserve(signed_txs.size());
  for (size_t i = 0; i < signed_txs.size(); i++) {
    batch_res.push_back(
        co_await run_propose_transaction(std::move(signed_txs[i]),
                                         first_tx_id + i));
  }
  assert(batch_res.size() == signed_txs.size());
  co_return std::move(batch_res);
}

x23::Async<ExpectedViewResult>
ExecutorContext::view_transaction(TransactionViewQuery const &query)
{
  assert(mode_ == ExecutionMode::kViewing);
  // Get off the RPC reader before executing.  x23's reader coroutine calls
  // serve() inline, so without this hop the body runs on the reader up to its
  // first database read and every other request on that connection waits
  // behind it -- and a connection serves many transactions at once.  main did
  // not have this problem because it submitted to a DispatchPool and parked on
  // the result, which returned to the reader immediately.
  co_await default_threadpool().bg();
  // query stays borrowed from the caller across the hop; the RPC service keeps
  // the request alive until the returned Async completes.
  co_return co_await run_view_transaction(query);
}

x23::Async<ExecValidateBatchResult>
ExecutorContext::validate_transaction_batch(
    ProposedTransactionBatch proposed_txs, TransactionID first_tx_id)
{
  assert(mode_ == ExecutionMode::kValidating);
  // Get off the RPC reader before executing.  x23's reader coroutine calls
  // serve() inline, so without this hop the body runs on the reader up to its
  // first database read and every other request on that connection waits
  // behind it -- and a connection serves many transactions at once.  main did
  // not have this problem because it submitted to a DispatchPool and parked on
  // the result, which returned to the reader immediately.
  co_await default_threadpool().bg();
  ExecValidateBatchResult batch_res;
  batch_res.reserve(proposed_txs.size());
  for (size_t i = 0; i < proposed_txs.size(); i++) {
    batch_res.push_back(
        co_await run_validate_transaction(std::move(proposed_txs[i]),
                                          first_tx_id + i));
  }
  assert(batch_res.size() == proposed_txs.size());
  co_return std::move(batch_res);
}

x23::Async<ExecResult>
ExecutorContext::validate_transaction(ProposedTransaction proposed_tx,
                                      TransactionID tx_id)
{
  assert(mode_ == ExecutionMode::kValidating);
  // Get off the RPC reader before executing.  x23's reader coroutine calls
  // serve() inline, so without this hop the body runs on the reader up to its
  // first database read and every other request on that connection waits
  // behind it -- and a connection serves many transactions at once.  main did
  // not have this problem because it submitted to a DispatchPool and parked on
  // the result, which returned to the reader immediately.
  co_await default_threadpool().bg();
  co_return co_await run_validate_transaction(std::move(proposed_tx), tx_id);
}

// Stale-block execution during propose/validate is benign.
//
// The StorageSnapshotCache can mask HogDb stale-block detection: if
// deployment metadata, contract code, or storage items are already cached,
// later reads return the cached value without hitting HogDb and the
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
x23::Async<ExecProposeResult>
ExecutorContext::run_propose_transaction(SignedTransaction signed_tx,
                                         TransactionID tx_id)
{
  static thread_local auto context = make_hog_exec_context();
  assert(mode_ == ExecutionMode::kProposing);

  // Cap the body, not the wrapper: signatures are node-local (#3844).
  if (x23::xdr_size(signed_tx.tx) > kMaxBytesTransaction) {
    co_return ExecProposeResult(ExecResult::ExecResultMalformedTransaction);
  }

  auto proxy = make_per_tx_db_proxy(tx_id);
  // SystemCallHandler's ctor re-serializes the tx for sha256 (#1905), which
  // allocates proportional to attacker-influenced input. An exception
  // escaping an x23 coroutine body is fatal (trace_unhandled_exception is
  // [[noreturn]]), so contain bad_alloc here; anything else is a genuine bug
  // and should still terminate.
  //
  // The handler borrows signed_tx.tx.contracts_to_deploy rather than deep-
  // copying it (the #1538 copy was dropped deliberately): it holds pointers
  // into the vector's elements until try_commit_transaction() returns. The
  // std::move(signed_tx) below is safe because it happens after commit and
  // x23::Vec's move steals the heap buffer, so the elements stay alive (at
  // stable addresses) in the event coroutine's body for as long as the
  // handler does.
  std::optional<SystemCallHandler> syscall_handler;
  try {
    syscall_handler.emplace(signed_tx.tx, *snapshot_cache_, std::move(proxy),
                            make_per_tx_event_server_proxy(tx_id),
                            ProposalModeCtor{}, *context);
  } catch (std::bad_alloc const &e) {
    // The diagnostic itself allocates while formatting and can throw again
    // under the same memory pressure, so guard it and fall back to the
    // no-allocation reporter (cf. database_proxy.cc commit_transaction).
    try {
      error("Unrecoverable: SystemCallHandler construction failed: {}",
            e.what());
    } catch (...) {
      report_dropped_log("SystemCallHandler ctor failure diagnostic");
    }
    co_return ExecProposeResult(ExecResult::ExecResultUnrecoverable);
  }
  co_await syscall_handler->root_invoke();
  auto exec_res = co_await syscall_handler->try_commit_transaction();

  auto out = ExecProposeResult(exec_res);

  switch (exec_res) {
  case ExecResult::ExecResultOK:
  case ExecResult::ExecResultTxDeterministicError:
  case ExecResult::ExecResultTxRootFrameReverted:
    // Committed, so the handler is done with its proposal data: move it into
    // the reply. The event emission below still needs its own copy.
    out.proposal_data() = syscall_handler->extract_proposal_data();
    break;
  case ExecResult::ExecResultTxNonDeterministicError:
  case ExecResult::ExecResultFailedGasSponsorship:
  case ExecResult::ExecResultTxValidationFailure:
  case ExecResult::ExecResultMalformedTransaction:
  case ExecResult::ExecResultSystemError:
  case ExecResult::ExecResultUnrecoverable:
    // nothing to do: a rejected transaction (e.g. an expired uniquifier, #2568,
    // which surfaces as a ValidationFailure) is not included in the proposal.
    break;
  }

  if (exec_res == ExecResult::ExecResultOK ||
      exec_res == ExecResult::ExecResultTxDeterministicError ||
      exec_res == ExecResult::ExecResultTxRootFrameReverted) {
    // Move signed_tx (unused after this) so the attacker-influenced tx is not
    // deep-copied into the event coroutine's body.
    ProposedTransaction proposed_tx{
        .signed_tx = std::move(signed_tx),
        .nondeterministic_proposal_data = out.proposal_data(),
    };
    spawn_event_emission(*syscall_handler, exec_res, std::move(proposed_tx));
  }

  co_return out;
}

x23::Async<ExecResult>
ExecutorContext::run_validate_transaction(ProposedTransaction tx,
                                          TransactionID tx_id)
{
  static thread_local auto context = make_hog_exec_context();
  assert(mode_ == ExecutionMode::kValidating);
  // Same body cap as proposal (#3844).
  if (x23::xdr_size(tx.signed_tx.tx) > kMaxBytesTransaction) {
    co_return ExecResult::ExecResultTxValidationFailure;
  }
  auto proxy = make_per_tx_db_proxy(tx_id);
  // SystemCallHandler's ctor re-serializes the tx for sha256 (#1905), which
  // allocates proportional to attacker-influenced input. An exception
  // escaping an x23 coroutine body is fatal (trace_unhandled_exception is
  // [[noreturn]]), so contain bad_alloc here; anything else is a genuine bug
  // and should still terminate.
  //
  // The handler borrows tx.signed_tx.tx.contracts_to_deploy rather than deep-
  // copying it (the #1538 copy was dropped deliberately): it holds pointers
  // into the vector's elements until try_commit_transaction() returns. The
  // std::move(tx) below is safe because it happens after commit and
  // x23::Vec's move steals the heap buffer, so the elements stay alive (at
  // stable addresses) in the event coroutine's body for as long as the
  // handler does.
  std::optional<SystemCallHandler> syscall_handler;
  try {
    syscall_handler.emplace(tx.signed_tx.tx, *snapshot_cache_, std::move(proxy),
                            make_per_tx_event_server_proxy(tx_id),
                            ValidationModeCtor{
                                tx.nondeterministic_proposal_data},
                            *context);
  } catch (std::bad_alloc const &e) {
    // The diagnostic itself allocates while formatting and can throw again
    // under the same memory pressure, so guard it and fall back to the
    // no-allocation reporter (cf. database_proxy.cc commit_transaction).
    try {
      error("Unrecoverable: SystemCallHandler construction failed: {}",
            e.what());
    } catch (...) {
      report_dropped_log("SystemCallHandler ctor failure diagnostic");
    }
    co_return ExecResult::ExecResultUnrecoverable;
  }
  co_await syscall_handler->root_invoke();
  auto exec_res = co_await syscall_handler->try_commit_transaction();

  if (exec_res == ExecResult::ExecResultOK ||
      exec_res == ExecResult::ExecResultTxDeterministicError ||
      exec_res == ExecResult::ExecResultTxRootFrameReverted) {
    // [tx] is unused after this; move it so the attacker-influenced proposal
    // body is never deep-copied into the event coroutine.
    spawn_event_emission(*syscall_handler, exec_res, std::move(tx));
  }
  co_return exec_res;
}

x23::Async<ExpectedViewResult>
ExecutorContext::run_view_transaction(TransactionViewQuery const &query)
{
  assert(mode_ == ExecutionMode::kViewing);
  static thread_local auto context = make_hog_exec_context();
  co_return co_await View::run_view_transaction(
      *snapshot_cache_,
      make_per_tx_db_proxy(std::numeric_limits<TransactionID>::max()),
      make_per_tx_event_server_proxy(std::numeric_limits<TransactionID>::max()),
      query, *context, {});
}

x23::Detached
ExecutorContext::emit_and_release(SystemCallHandler &handler,
                                  ExecResult exec_res,
                                  ProposedTransaction proposed_tx)
{
  // co_await keeps the event coroutine owned here, so a later allocation can't
  // destroy it mid-flight. The emit_events and event-proxy ramps allocate their
  // frames inside this detached body, where an escaping bad_alloc would
  // terminate the executor; contain it so the failure is recorded as a block
  // event error instead. A caught bad_alloc only ever fires before the RPC
  // starts or after it has completed, never while one is in flight.
  // log_event_return releases the ledger on every path.
  try {
    log_event_return(
        co_await handler.emit_events(exec_res, std::move(proposed_tx)));
  } catch (std::bad_alloc const &) {
    log_event_return(
        SystemStatus<void>{SystemError{SystemErrorCode::Unrecoverable}});
  }
}

void
ExecutorContext::spawn_event_emission(SystemCallHandler &handler,
                                      ExecResult exec_res,
                                      ProposedTransaction proposed_tx)
{
  // Release: the bump must be visible before other threads see it; finish_block
  // drains the ledger at shutdown once acquire_reference() starts failing.
  pending_event_calls_.fetch_add(1, std::memory_order_release);
  // emit_and_release's own ramp frame allocates here, before its body runs; on
  // bad_alloc record a block event error and release the ledger. Allocations
  // inside the body are contained there, so no path strands the ledger.
  try {
    emit_and_release(handler, exec_res, std::move(proposed_tx));
  } catch (std::bad_alloc const &) {
    log_event_return(
        SystemStatus<void>{SystemError{SystemErrorCode::Unrecoverable}});
  }
}

void
ExecutorContext::log_event_return(SystemStatus<void> err)
{
  if (!err.has_value()) {
    std::unique_lock lock(event_accumulate_mutex_);
    event_error_ = err.error().code();
    // This runs in an x23::Detached callback, where an escaping exception is
    // fatal, and formatting the diagnostic can throw under memory pressure.
    // Guard it and fall back to the no-allocation reporter, same as the
    // SystemCallHandler ctor paths above; the decrement below still runs.
    try {
      error("event error: {}", std::to_underlying(event_error_.value()));
    } catch (...) {
      report_dropped_log("event error diagnostic");
    }
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
