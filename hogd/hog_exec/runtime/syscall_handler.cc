#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <exception>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "auth_token.hh"
#include "contract.hh"
#include "db_conflict.hh"
#include "event.hh"
#include "exec_result.hh"
#include "executor_to_db_rpc.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_event/utilities.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/execution_mode.h"
#include "hogd/hog_exec/components/new_contract_set.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/event_server_proxy.h"
#include "hogd/hog_exec/runtime/invocation_frame.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/item/frame_item_key.h"
#include "hogd/hog_exec/runtime/item/item_view.h"
#include "hogd/hog_exec/runtime/item/item_view_type.h"
#include "hogd/hog_exec/runtime/item/snapshot_storage_item.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hog_exec/runtime/metrics.h"
#include "hogd/hog_exec/runtime/prepared_contract.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "invoke_flag.hh"
#include "precompiles.hh"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/format.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

namespace {

size_t
contract_blob_size(Contract const &contract)
{
  switch (contract.contract_type()) {
  case ContractType::kNative:
  case ContractType::kMockTest:
    return 0;
  case ContractType::kRv32:
    return contract.rv32_contract().data.size();
  }
  panic("Unrecoverable; Invalid contract type: {}", contract.contract_type());
}

constexpr NewContractBatch kEmptyVec = {};

ExecutionModeData
make_execution_mode_data(ExecutionModeCtor execution_mode_ctor)
{
  if (std::holds_alternative<ProposalModeCtor>(execution_mode_ctor)) {
    return ProposalModeData{};
  }
  if (std::holds_alternative<ViewingModeCtor>(execution_mode_ctor)) {
    return ExecutionModeData{std::in_place_type<ViewingModeData>,
                             std::get<ViewingModeCtor>(execution_mode_ctor)};
  }
  return ExecutionModeData{std::in_place_type<ValidationModeData>,
                           std::get<ValidationModeCtor>(execution_mode_ctor)};
}

// Run a logging statement that may itself throw (allocation or sink failure)
// behind a noexcept boundary. Returns false if the log was dropped, in which
// case dropped_desc reaches the no-allocation reporter instead.
template<typename F>
bool
guarded_log(char const *dropped_desc, F &&log_fn) noexcept
{
  try {
    std::forward<F>(log_fn)();
    return true;
  } catch (...) {
    report_dropped_log(dropped_desc);
    return false;
  }
}

// Populate an UnmodifiedItem's base item from the snapshot cache if it has
// not been loaded yet. Not a coroutine: the already-loaded arm answers with a
// plain value, and the load arm forwards load()'s own MaybeAsync, preserving
// its no-frame-on-cache-hit property.
x23::MaybeAsync<SystemStatus<void>>
ensure_loaded(UnmodifiedItem const &item, StorageSnapshotCache &snapshot_cache,
              FrameItemKey const &item_key)
{
  if (item.is_loaded()) {
    return SystemStatus<void>{};
  }
  return item.load(snapshot_cache, item_key);
}

} // namespace

ExecResult
SystemCallHandler::to_exec_result(InvokeErrorCode error_code)
{
  switch (error_code) {
  case InvokeErrorCode::None:
    return ExecResult::ExecResultOK;
  case InvokeErrorCode::Abandon:
    return ExecResult::ExecResultSystemError;
  case InvokeErrorCode::Deterministic:
  case InvokeErrorCode::OutOfGas:
    return ExecResult::ExecResultTxDeterministicError;
  case InvokeErrorCode::ValidationFailure:
    return ExecResult::ExecResultTxValidationFailure;
  case InvokeErrorCode::Unrecoverable:
    error("make_exec_result: received Unrecoverable");
    return ExecResult::ExecResultUnrecoverable;
  }
  assert(false);
  return ExecResult::ExecResultUnrecoverable;
}

ExecResult
SystemCallHandler::to_exec_result(SystemErrorCode error_code)
{
  switch (error_code) {
  case SystemErrorCode::Unrecoverable:
    return ExecResult::ExecResultUnrecoverable;
  case SystemErrorCode::Abandon:
    return ExecResult::ExecResultSystemError;
  case SystemErrorCode::ValidationFailure:
    return ExecResult::ExecResultTxValidationFailure;
  }
  assert(false);
  return ExecResult::ExecResultUnrecoverable;
}

bool
SystemCallHandler::replay_uniquifier_expired(Uniquifier const &uniquifier,
                                             BlockHeight executing_height)
{
  // ReplayCacheLock rejects a uniquifier whose block_expiry_height is strictly
  // less than the executing block's height.
  return uniquifier.block_expiry_height < executing_height;
}

bool
SystemCallHandler::replay_uniquifier_expired() const
{
  // Only real transactions carry a uniquifier (the test-harness constructor
  // leaves it empty).
  if (!tx_uniquifier_.has_value()) {
    return false;
  }
  // get_block_height() returns the executing block height (parent tip + 1),
  // which is exactly the height ReplayCacheLock validates expiry against.
  return replay_uniquifier_expired(*tx_uniquifier_, get_block_height());
}

// TODO(geoff): Setup configuration parameters for alternative contract
// runtimes and/or shared contexts.
SystemCallHandler::SystemCallHandler(
    Transaction const &tx, StorageSnapshotCache &snapshot_cache,
    PerTransactionDatabaseProxy db_proxy,
    PerTransactionEventServerProxy event_server_proxy,
    ExecutionModeCtor execution_mode_ctor, RuntimeContext &runtime_context)
  : runtime_context_(runtime_context), db_proxy_(std::move(db_proxy)),
    event_server_proxy_(std::move(event_server_proxy)),
    invocation_frame_stack_(
        [this](InvocationFrame &&last_frame) {
          commit_last_call_frame_success(std::move(last_frame));
        },
        db_proxy_, snapshot_cache.get_network_config_params()),
    source_tx_hash_(sha256(tx)), auth_claim_(tx.auth_claim),
    root_invocation_(tx.invocation), tx_uniquifier_(tx.uniquifier),
    root_invoked_address_(tx.invocation.invoked_address),
    execution_mode_data_(
        make_execution_mode_data(std::move(execution_mode_ctor))),
    contracts_to_deploy_(tx.contracts_to_deploy),
    snapshot_cache_(snapshot_cache)
{
  // Real transactions execute in proposal or validation mode only; viewing
  // mode goes through the tx-less constructor. CHECK, not assert: the old
  // per-ctor construction failed loudly (bad_variant_access) on this misuse
  // in release builds too.
  CHECK(!std::holds_alternative<ViewingModeData>(execution_mode_data_));
}

SystemCallHandler::SystemCallHandler(
    StorageSnapshotCache &snapshot_cache, PerTransactionDatabaseProxy db_proxy,
    PerTransactionEventServerProxy event_server_proxy,
    ExecutionModeCtor execution_mode_ctor, RuntimeContext &runtime_context,
    AuthClaim auth_claim, SHA256Hash source_tx_hash)
  : runtime_context_(runtime_context), db_proxy_(std::move(db_proxy)),
    event_server_proxy_(std::move(event_server_proxy)),
    invocation_frame_stack_(
        [this](InvocationFrame &&last_frame) {
          commit_last_call_frame_success(std::move(last_frame));
        },
        db_proxy_, snapshot_cache.get_network_config_params()),
    source_tx_hash_(source_tx_hash), auth_claim_(std::move(auth_claim)),
    execution_mode_data_(
        make_execution_mode_data(std::move(execution_mode_ctor))),
    contracts_to_deploy_(kEmptyVec), snapshot_cache_(snapshot_cache)
{}

std::pair<InvokeStatus<void>, GasAmount>
SystemCallHandler::perform_initial_validation()
{
  // Reject a transaction whose replay uniquifier has already expired before any
  // attached-contract verification or body execution runs (#2568). Without this
  // the body would run to completion and the transaction would only be rejected
  // by the late replay-cache reservation.
  if (replay_uniquifier_expired()) {
    // An expired uniquifier is a deterministic, proposer-attributable
    // rejection: map it to ValidationFailure uniformly in proposal and
    // validation mode. In proposal mode this is only reachable when the
    // scheduler tried to propose a transaction it should have already dropped
    // -- try_commit_transaction() asserts (debug only) that any proposal-mode
    // ValidationFailure came from this initial validation.
    return {InvokeError{InvokeErrorCode::ValidationFailure}, 0};
  }

  // Validate the proposal data structure before execution begins.
  // This checks well-formedness (e.g. bit count matches byte count,
  // no entropy grinding via padding bits).
  if (std::holds_alternative<ValidationModeData>(execution_mode_data_)) {
    auto &validation_data = std::get<ValidationModeData>(execution_mode_data_);
    if (!validation_data.validate()) {
      return {InvokeError{InvokeErrorCode::ValidationFailure}, 0};
    }
  }

  // Without this gate an oversized attachment set fails only at commit, where
  // it reports as unrecoverable (#2190). Deterministic from the tx bytes.
  if (const uint32_t permitted = config_params().max_new_contracts_per_tx;
      contracts_to_deploy_.size() > permitted) {
    error("Transaction attaches {} contracts; chain permits {}",
          contracts_to_deploy_.size(), permitted);
    return {InvokeError{InvokeErrorCode::ValidationFailure}, 0};
  }

  // The blob bound is the chain's (#2569): a configuration may raise it past
  // the default, and hog_db stores whatever a loadable genesis admits.
  for (auto const &contract : contracts_to_deploy_) {
    if (const uint32_t permitted = config_params().max_contract_size_bytes;
        contract_blob_size(contract) > permitted) {
      error("Transaction attaches a {}-byte contract; chain permits {}",
            contract_blob_size(contract), permitted);
      return {InvokeError{InvokeErrorCode::ValidationFailure}, 0};
    }
  }

  // make() allocates proportional to attacker-influenced input (the maps and
  // each rv32 translation), and this runs inside an x23 coroutine where an
  // escaping throw is fatal. The old attachment deep copy ran in the ctor
  // under executor_context's bad_alloc containment; that containment moves
  // here with the work. Contain bad_alloc only -- allocation-free, so the
  // handler itself cannot rethrow -- and keep anything else terminating,
  // since it is a genuine bug.
  try {
    auto [res, gas_consumed] =
        NewContractSet::make(contracts_to_deploy_, runtime_context_,
                             config_params());
    if (!res.has_value()) {
      return {res.error(), gas_consumed};
    }
    newly_created_contracts_ = std::move(*res);

    return {InvokeStatus<void>{}, gas_consumed};
  } catch (std::bad_alloc const &) {
    return {InvokeError{InvokeErrorCode::Unrecoverable}, 0};
  }
}

bool
SystemCallHandler::check_final_validation_data() const
{
  if (auto const *validation_data =
          std::get_if<ValidationModeData>(&execution_mode_data_)) {
    return validation_data->check_all_consumed();
  }
  return true;
}

x23::Async<InvokeResult>
SystemCallHandler::root_invoke()
{
  // A system error here (e.g. an unreadable db tip during the replay-expiry
  // check) is surfaced as the corresponding invoke error rather than dropped.
  //
  // TODO(geoff): gas cost calibration -- verification gas is hardwired to 0
  // today. Both paths already record it: the failure branch reports it
  // directly, and the success path deducts it from the frame's available gas
  // and adds it back into the final gas_consumed_.
  auto [initial_validation_res, initial_validation_gas_consumed] =
      perform_initial_validation();
  if (!initial_validation_res.has_value()) {
    // The root invocation is terminally rejected before any frame ran (e.g. an
    // expired uniquifier or a failed contract verification).
    // finalize_after_root_frame() marks it finished/finalized so callers treat
    // it as a completed-but-failed root, letting the rejected transaction
    // surface its ExecResult via try_commit_transaction().
    initial_validation_rejected_ = true;
    co_return finalize_after_root_frame(
        InvokeResult{.result_ = initial_validation_res.error(),
                     .gas_consumed_ = initial_validation_gas_consumed});
  }

  // root_invocation_ is nullopt only in the test harness
  CHECK(root_invocation_.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  Invocation root_invocation = std::move(root_invocation_.value());
  root_invocation_.reset();

  // The frame's gas is bounded by available_gas (runtime.cc checks
  // gas_consumed <= gas_limit), so the reported total below stays within
  // gas_limit -- except when initial_validation_gas_consumed alone reaches
  // gas_limit, where the total is the unclamped validation gas.
  // TODO(geoff): when gas calibration lands, bound the work done in initial
  // validation so its gas cannot exceed gas_limit (or reject/clamp when it
  // does), restoring gas_consumed_ <= gas_limit unconditionally.
  GasAmount available_gas =
      initial_validation_gas_consumed < root_invocation.gas_limit
          ? root_invocation.gas_limit - initial_validation_gas_consumed
          : 0;

  auto [res, gas_consumed] =
      co_await invoke(root_invocation.invoked_address,
                      sdk_method_id(root_invocation.entrypoint), available_gas,
                      std::move(root_invocation.calldata),
                      std::to_underlying(InvokeFlag::None), {});

  co_return finalize_after_root_frame(
      InvokeResult{.result_ = handle_root_host_fn_status(res),
                   .gas_consumed_ =
                       gas_consumed + initial_validation_gas_consumed});
}

InvokeResult
SystemCallHandler::finalize_after_root_frame(InvokeResult result)
{
  assert(!finalized_);
  assert(invocation_frame_stack_.empty());
  finalized_ = true;
  // The root invocation is complete, whether a frame ran (finish_frame already
  // set this when the stack emptied) or the invoke was rejected before one
  // started. Owning the flag here keeps "finalized" and "last frame finished"
  // in lockstep: no further frames may start, get_most_recent_frame_output()
  // reports the finished (or absent) root frame, and the test FFI wrapper's
  // commit gate on last_frame_finished() admits the finalized result.
  last_frame_finished_ = true;

  root_invoke_result_ = result;

  // A failed final validation check rewrites exactly the results a frame can
  // legitimately produce -- success or a deterministic fault, the partition
  // to_frame_status() reports as a frame status -- to ValidationFailure.
  // Block-ending system errors (Abandon, Unrecoverable, an existing
  // ValidationFailure) are preserved.
  if (!check_final_validation_data() &&
      root_invoke_result_.to_frame_status().first.has_value()) {
    root_invoke_result_.result_ =
        InvokeError{InvokeErrorCode::ValidationFailure};
  }

  return root_invoke_result_;
}

InvocationFrameOutput
SystemCallHandler::get_most_recent_frame_output() const
{
  if (last_frame_finished_) {
    assert(invocation_frame_stack_.empty());

    if (!last_frame_.has_value()) {
      return InvocationFrameOutput(InvocationFrameStatusCode::Fault);
    }
    return last_frame_.value().get_frame_output();
  }

  // No frame finished and none is active: the (root) invoke was rejected before
  // any frame started (e.g. an undeployed target or a pre-frame reentrance /
  // input rejection). No frame produced output, so it faulted.
  if (invocation_frame_stack_.empty()) {
    return InvocationFrameOutput(InvocationFrameStatusCode::Fault);
  }

  assert(invocation_frame_stack_.get_active_frame_child_return_output()
             .has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  return *invocation_frame_stack_.get_active_frame_child_return_output();
}

x23::Vec<TxEmittedEvent>
SystemCallHandler::get_most_recent_frame_events() const
{
  if (last_frame_finished_) {
    assert(invocation_frame_stack_.empty());
    if (!last_frame_.has_value()) {
      return x23::Vec<TxEmittedEvent>();
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    if (last_frame_.value().should_revert()) {
      return x23::Vec<TxEmittedEvent>();
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return last_frame_.value().get_events();
  }
  // No frame finished and none is active (invoke rejected before any frame
  // started): there are no frame events to report. Kept symmetric with
  // get_most_recent_frame_output() so both handle the pre-frame state.
  if (invocation_frame_stack_.empty()) {
    return x23::Vec<TxEmittedEvent>();
  }
  return invocation_frame_stack_.copy_active_frame_events();
}

InvokeStatus<void>
SystemCallHandler::handle_root_host_fn_status(
    SystemStatus<SyscallStatus<InvocationFrameStatusCode>> status)
{
  if (status.has_value()) {
    SyscallStatus<InvocationFrameStatusCode> const &frame = status.value();
    // An invoke rejected before a child frame ran (e.g. reentrance, invalid
    // input) is a top-level deterministic failure.
    if (!frame.has_value()) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    // TODO(geoff): Consider whether we want to have "frame reverted" on the
    // root frame as a "transaction failed" or "transaction succeeded"
    switch (frame.value()) {
    case InvocationFrameStatusCode::Return:
    case InvocationFrameStatusCode::Abort:
      return InvokeStatus<void>{};
    case InvocationFrameStatusCode::Fault:
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    error("handle_root_host_fn_status: received unknown frame status {}",
          std::to_underlying(frame.value()));
    return InvokeError{InvokeErrorCode::Unrecoverable};
  }
  switch (status.error().code()) {
  case SystemErrorCode::ValidationFailure:
    return InvokeError{InvokeErrorCode::ValidationFailure};
  case SystemErrorCode::Abandon:
    return InvokeError{InvokeErrorCode::Abandon};
  case SystemErrorCode::Unrecoverable:
    error("handle_root_host_fn_status: received Unrecoverable");
    return InvokeError{InvokeErrorCode::Unrecoverable};
  }
  // Leaving the default arm out of the switch, so that the compiler
  // will raise a warning later if we add additional InvokeError codes.
  error("handle_root_host_fn_status: received unknown error code {}",
        status.error().code());
  return InvokeError{InvokeErrorCode::Unrecoverable};
}

x23::Async<ExecResult>
SystemCallHandler::abort_after_commit_failure(ExecResult failure_result,
                                              bool report_diagnostic)
{
  two_phase_commit_failed_ = true;
  if (report_diagnostic) {
    report_db_conflict_diagnostic();
  }
  // abort_transaction() marks tx_failed and sets
  // transaction_committed_or_aborted_. A failed abort (e.g. an unreadable db)
  // outranks the commit failure being reported.
  auto abort_status = co_await abort_transaction();
  if (!abort_status.has_value()) {
    co_return to_exec_result(abort_status.error().code());
  }
  co_return failure_result;
}

x23::Async<ExecResult>
SystemCallHandler::try_commit_transaction()
{
  assert(!transaction_committed_or_aborted_);

  if (!root_invoke_result_.is_root_success()) {
    auto error_code = root_invoke_result_.result_.error().code();
    if (error_code == InvokeErrorCode::Unrecoverable) {
      error("try_commit_transaction: received Unrecoverable");
    }

    // Deterministic faults (including OutOfGas) are still included in the
    // block. Their body effects are discarded, but the transaction must still
    // commit its per-tx side-records: RCQ observations (so block-close matching
    // can reject forged observations on a faulting tx) and the replay nonce (so
    // the transaction cannot be replayed). The body savepoints were already
    // popped by mark_frame_as_failed during frame unwinding, so this commits a
    // clean transaction containing only those side-records.
    if (error_code == InvokeErrorCode::Deterministic ||
        error_code == InvokeErrorCode::OutOfGas) {
      co_return co_await commit_body_discarded(to_exec_result(error_code));
    }

    // Proposal mode should never reject a transaction with a ValidationFailure
    // unless perform_initial_validation() rejected it (an expired replay
    // uniquifier or a malformed attachment set): the scheduler must not have
    // tried to propose such a transaction in the first place, and no mid-body
    // execution path may produce one. Assert that (debug only) so a
    // proposal-mode validation failure introduced elsewhere can't slip through
    // silently.
    if constexpr (is_debug_build()) {
      if (error_code == InvokeErrorCode::ValidationFailure &&
          std::holds_alternative<ProposalModeData>(execution_mode_data_)) {
        CHECK(initial_validation_rejected_);
      }
    }

    // System / unrecoverable / validation errors: the transaction is not
    // included and consumes nothing.
    auto res = co_await abort_transaction();
    if (!res.has_value()) {
      co_return to_exec_result(res.error().code());
    }

    co_return to_exec_result(error_code);
  }
  assert(last_frame_.has_value());
  assert(root_invoke_succeeded());
  assert(root_invoke_result_.result_.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  InvocationFrame &last_frame = last_frame_.value();

  // If the last frame was set to manually revert, then we commit the
  // transaction as a no-op: body effects are discarded, but RCQ observations
  // and the replay nonce are still committed (see commit_body_discarded).
  if (last_frame.should_revert()) {
    // Extract the events so that the reverted transaction's events cannot be
    // sent later to the event server.
    last_frame.extract_events();
    co_return co_await commit_body_discarded(
        ExecResult::ExecResultTxRootFrameReverted);
  }

  // Root commit: push the accumulated delta batch in one HogDb subtransaction.
  // The child TryInvoke depth cap reserves one slot for this push.
  //
  // Validation mode pushes no per-frame subtransactions, so the root frame's
  // try_invoke_frame_count_ must still be 0 here. This is load-bearing: if a
  // future refactor bumps it in validation, mark_frame_as_failed on a later
  // failure path would pop_subtransactions a savepoint that was never pushed.
  assert(!std::holds_alternative<ValidationModeData>(execution_mode_data_) ||
         last_frame.get_try_invoke_frame_count() == 0);

  auto const &observations = std::visit(
      [](auto const &mode)
          -> std::map<AddressAndKey, std::vector<QueueEntry>> const & {
        return mode.observations_to_validate();
      },
      execution_mode_data_);

  // Add the replay-nonce consume delta to the batch before acquiring the body
  // locks, so it rides in the same subtransaction push. A duplicate or expired
  // nonce makes try_acquire_frame_locks fail, rejecting the tx as a replay.
  add_replay_nonce_delta();

  auto r = co_await last_frame.try_acquire_frame_locks(db_proxy_, observations);
  if (!r.has_value()) {
    co_return co_await abort_after_commit_failure(to_exec_result(
                                                      r.error().code()),
                                                  /*report_diagnostic=*/true);
  }

  // check if any of the 2pc database operations failed
  if (!r.value()) {
    co_return co_await abort_after_commit_failure(
        ExecResult::ExecResultTxNonDeterministicError,
        /*report_diagnostic=*/true);
  }

  // Abort on a throw here rather than let it escape with the tx still open: a
  // retry would re-push the replay-cache delta and hard-abort at narrow
  // (#2595).
  //
  // The abort cannot run inside the catch handler -- co_await is illegal there
  // -- so both failure paths record the outcome and the shared abort runs just
  // below, still before anything else can observe the open transaction.
  std::optional<ExecResult> commit_failure;
  try {
    auto new_contract_hashes = last_frame.get_committed_contracts();
    auto res = co_await db_proxy_.commit_transaction(
        newly_created_contracts_.borrow_contracts(new_contract_hashes));
    if (!res.has_value()) {
      commit_failure = to_exec_result(res.error().code());
    }
  } catch (std::exception const &e) {
    error("try_commit_transaction: commit preparation threw: {}", e.what());
    commit_failure = ExecResult::ExecResultUnrecoverable;
  }
  if (commit_failure.has_value()) {
    co_return co_await abort_after_commit_failure(*commit_failure,
                                                  /*report_diagnostic=*/false);
  }

  // This is past the point of successful commitment;
  // now we can safely emit the events to the event server

  HogexecMetrics::mark_tx_success(root_invoke_result_,
                                  get_block_execution_mode());
  transaction_committed_or_aborted_ = true;
  co_return ExecResult::ExecResultOK;
}

x23::Async<SystemStatus<void>>
SystemCallHandler::abort_transaction()
{
  assert(!transaction_committed_or_aborted_);
  transaction_committed_or_aborted_ = true;
  // Discard any events the (successful) root frame produced: an aborted
  // transaction never publishes them, and emit_events() -- the only path that
  // drains them on commit -- is not reached here. Leaving them in last_frame_
  // would otherwise trip the debug destructor's "destroyed with events to emit"
  // panic when an honest test FFI caller closes an uncommitted handler after an
  // event-emitting root_invoke (issue #2454).
  if (last_frame_.has_value()) {
    last_frame_.value().extract_events();
  }
  HogexecMetrics::mark_tx_failed(root_invoke_result_,
                                 get_block_execution_mode());
  co_return co_await db_proxy_.abort_transaction();
}

void
SystemCallHandler::add_replay_nonce_delta()
{
  // Only real transactions (constructed from a Transaction) carry a uniquifier;
  // the test-harness constructor leaves it empty and consumes no nonce.
  if (!tx_uniquifier_.has_value()) {
    return;
  }
  // tx_uniquifier_ and root_invoked_address_ are populated together by the
  // Transaction constructor; a uniquifier without a root address would make the
  // access below undefined. Make that invariant explicit.
  assert(root_invoked_address_.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  db_proxy_.add_delta(AddressAndKey{.address = root_invoked_address_.value(),
                                    .key = StorageKey(
                                        StorageKeyType::ReplayCache)},
                      ReplayCacheItem{tx_uniquifier_.value()}.get_delta(),
                      std::to_underlying(StorageDeltaFlags::None));
}

x23::Async<ExecResult>
SystemCallHandler::commit_body_discarded(ExecResult success_result)
{
  // In validation mode, RCQ observation deltas must reach HogDb so block-close
  // matching can reject forged observations. The normal success path handles
  // this inside try_acquire_frame_locks(); a manually-reverted or faulting
  // transaction discards its body, so it must submit observation deltas here.
  // (In proposal mode observations_to_validate() is empty.)
  if (std::holds_alternative<ValidationModeData>(execution_mode_data_)) {
    auto const &observations =
        std::get<ValidationModeData>(execution_mode_data_)
            .observations_to_validate();
    for (auto const &[addrkey, entries] : observations) {
      if (!entries.empty()) {
        StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
        delta.retrocausal_queue().observed_concurrent_entries = {
            entries.begin(), entries.end()};
        db_proxy_.add_delta(addrkey, std::move(delta),
                            std::to_underlying(StorageDeltaFlags::None));
      }
    }
  }

  // The replay nonce rides in the same batch as the observation deltas: a
  // duplicate/expired nonce (or a forged observation) fails the reservation and
  // the transaction is not included.
  add_replay_nonce_delta();

  auto push_res = co_await db_proxy_.try_push_current_subtransaction();
  if (!push_res.has_value()) {
    // A system error (e.g. a stale block) leaves the HogDb InvokeTX open; abort
    // it before returning, mirroring the root-success lock-push failure path.
    co_return co_await abort_after_commit_failure(to_exec_result(
                                                      push_res.error().code()),
                                                  /*report_diagnostic=*/false);
  }
  if (!push_res.value()) {
    co_return co_await abort_after_commit_failure(
        ExecResult::ExecResultTxNonDeterministicError,
        /*report_diagnostic=*/true);
  }

  auto res = co_await db_proxy_.commit_transaction();
  if (!res.has_value()) {
    // A system error (e.g. a stale block) leaves the HogDb InvokeTX open; abort
    // it before returning, mirroring the push-failure paths above and the
    // success-path commit failure in try_commit_transaction().
    co_return co_await abort_after_commit_failure(to_exec_result(
                                                      res.error().code()),
                                                  /*report_diagnostic=*/false);
  }

  HogexecMetrics::mark_tx_failed(root_invoke_result_,
                                 get_block_execution_mode());
  transaction_committed_or_aborted_ = true;
  co_return success_result;
}

// can only be called when call_frame_stack_ is nonempty
// (i.e. while at least one invoke() is running)
const ContractAddress &
SystemCallHandler::get_current_address() const
{
  return invocation_frame_stack_.get_current_address();
}

SHA256Hash
SystemCallHandler::get_source_tx_hash() const
{
  return source_tx_hash_;
}

SyscallStatus<ContractAddress>
SystemCallHandler::get_msg_sender() const
{
  assert(!invocation_frame_stack_.empty());
  auto res = invocation_frame_stack_.get_msg_sender();
  return res.has_value()
             ? SyscallStatus<ContractAddress>(*res)
             : SyscallError{
                   SyscallErrorCode::kSyscallErrorCodeIsRootTransaction};
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::validate_new_invocation(
    ContractAddress const &address, InvokeFlagSet flags,
    std::span<uint32_t const> owned_auth_token_ids) const
{
  if (!validate_invoke_flags(flags)) {
    return SyscallStatus<void>{std::unexpected(
        SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput})};
  }
  // Reentrance and the depth bound are both conditions of the calling frame,
  // and both sit above the auth-token check so that either is reported even
  // when the caller also forwarded a token it does not own (#2973 established
  // that for reentrance).
  //
  // Reentrance first: it says the call can never succeed anywhere in this
  // chain, where the depth bound says only that this frame is too deep and a
  // shallower one would admit the same call.
  if (invocation_frame_stack_.has_active_frame_for(address)) {
    return SyscallStatus<void>{std::unexpected(
        SyscallError{SyscallErrorCode::kSyscallErrorCodeReentrance})};
  }
  // A chain bounds how many invocation frames one transaction may have open at
  // once. Every open frame holds a live contract runtime, so without this a
  // recursive call chain grows until the executor cannot hold it.
  //
  // The bound is a plain load off the tip the snapshot cache loaded when the
  // block was opened; get_network_config_params() fails loudly on a cache
  // whose tip was never loaded.
  if (invocation_frame_stack_.size() >=
      config_params().max_cross_invoke_depth) {
    return SyscallStatus<void>{std::unexpected(
        SyscallError{SyscallErrorCode::kSyscallErrorCodeInvokeDepthExceeded})};
  }
  if (!invocation_frame_stack_.empty() &&
      !invocation_frame_stack_.check_auth_tokens_owned(owned_auth_token_ids)) {
    return SyscallStatus<void>{std::unexpected(
        SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput})};
  }
  return SyscallStatus<void>{};
}

x23::Async<std::pair<SystemStatus<SyscallStatus<InvocationFrameStatusCode>>,
                     GasAmount>>
SystemCallHandler::invoke(ContractAddress const &address,
                          Runtime::MethodID method, GasAmount gas_limit,
                          Calldata calldata, InvokeFlagSet flags,
                          std::span<uint32_t const> owned_auth_token_ids)
{
  using invoke_result_t =
      std::pair<SystemStatus<SyscallStatus<InvocationFrameStatusCode>>,
                GasAmount>;

  // syscall_Invoke_inner() rejects undefined flags before we get here, so
  // reaching this with invalid ones is a bug, not caller input. Asserted here
  // even though validate_new_invocation() also checks it -- that check is for
  // the test FFI boundary, which must report rather than abort.
  CHECK(validate_invoke_flags(flags));

  if (!invocation_frame_stack_.empty() && is_read_only()) {
    flags |= std::to_underlying(InvokeFlag::ReadOnly);
  }

  // Rejected before a child frame is created, so this is a complete no-op:
  // nothing is mutated, and the caller leaves its output state untouched.
  // Every one of these -- the depth bound included -- is reported as a syscall
  // error, which is what CopyableInvokeOutputs documents a nonzero error code
  // to mean. Reporting a faulted child frame instead would be
  // indistinguishable from a callee that ran and trapped. (The TryInvoke cap
  // does report Fault, but it fires in InvocationFrameStack::finish_frame,
  // after its child frame has actually run.)
  auto admitted = validate_new_invocation(address, flags, owned_auth_token_ids);
  if (!admitted.has_value()) {
    co_return invoke_result_t{admitted.error(), 0};
  }
  if (!admitted->has_value()) {
    co_return invoke_result_t{admitted->error(), 0};
  }

  // resolve_runtime() reads the block-scoped deployment cache and, on a miss,
  // inserts a fresh (possibly negative) entry; that unordered_map insert can
  // throw std::bad_alloc under memory pressure. invoke() is reached from the
  // noexcept syscall_Invoke_inner() body, so the exception must not unwind
  // across that boundary and terminate the executor -- translate it into an
  // unrecoverable system error instead (issue #3363).
  //
  // Seeded with the result the handlers used to return: co_await is illegal in
  // a catch handler, so the arms below only log and leave this value in place.
  SystemStatus<std::unique_ptr<Runtime>> runtime =
      SystemError{SystemErrorCode::Unrecoverable};
  try {
    runtime = co_await resolve_runtime(address);
  } catch (std::exception const &e) {
    report_dropped_log(e.what());
  } catch (...) {
    report_dropped_log("unknown exception");
  }
  if (!runtime) {
    // invoke() is reached from the noexcept syscall_Invoke_inner() body. A
    // logger allocation/sink failure (typically std::bad_alloc out of memory)
    // must not throw across the noexcept boundary and terminate the caller;
    // fail the invocation as unrecoverable instead (issue #2714).
    if (!guarded_log("runtime-resolution failure diagnostic", [&] {
          trace("SystemError: resolving runtime for address: {}, error {}",
                x23::show(address), runtime.error());
        })) {
      co_return invoke_result_t{SystemError{SystemErrorCode::Unrecoverable}, 0};
    }
    co_return invoke_result_t{runtime.error(), 0};
  }
  if (!runtime.value()) {
    if (!guarded_log("runtime-instantiation failure diagnostic", [&] {
          trace("Failed to instantiate runtime for address: {}",
                x23::show(address));
        })) {
      co_return invoke_result_t{SystemError{SystemErrorCode::Unrecoverable}, 0};
    }
    co_return invoke_result_t{
        SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput},
        0};
  }

  Invocation invocation = {.invoked_address = address,
                           .entrypoint = entrypoint_from_method_id(method),
                           .gas_limit = gas_limit,
                           .calldata = std::move(calldata)};

  // Move (never copy) the Invocation into start_frame: copying its Calldata
  // allocates, and the argument is constructed here in invoke() -- reachable
  // from the noexcept syscall_Invoke_inner() -- before start_frame()'s
  // protected body runs, so a copy could throw std::bad_alloc across that
  // noexcept boundary and terminate the executor. The move is allocation-free
  // and invocation is unused afterward (#3058).
  auto start_status =
      start_frame(std::move(invocation), flags, owned_auth_token_ids);
  if (!start_status.has_value()) {
    // Child frame admission failed under memory pressure. start_frame() rolled
    // back the invocation stack, so no child frame is present and
    // finish_frame() must be skipped; surface the unrecoverable error (issue
    // #3058).
    co_return invoke_result_t{start_status.error(), 0};
  }
  auto stashed_parent_trace = std::move(start_status.value());

  InvokeResult cumulative_result =
      co_await runtime.value()->invoke(method, gas_limit);

  auto finish_status = co_await finish_frame(cumulative_result.result_,
                                             std::move(stashed_parent_trace));
  if (!finish_status.has_value()) {
    // TODO(geoff): This status will result ending execution of the whole block.
    // We can add gas consumption here, but that would only affect the state of
    // the local scheduler, not the canonical chain.
    co_return invoke_result_t{finish_status.error(), 0};
  }
  auto [frame_result, manually_reverted] = finish_status.value();
  cumulative_result += frame_result;

  // A child frame ran. Map its result to a frame status (the success value) or
  // propagate a SystemError. There is no SyscallError here: a frame that ran
  // never produces the "rejected before running" channel.
  auto [frame_status, gas_consumed] = cumulative_result.to_frame_status();
  if (!frame_status.has_value()) {
    co_return invoke_result_t{frame_status.error(), gas_consumed};
  }
  // to_frame_status() reports Return for any frame whose deterministic
  // execution succeeded; a manual revert (the contract called Return with
  // revert=true) is exactly that case with should_revert() set, so it maps to
  // Abort. A hard fault already reports Fault and is left unchanged
  // (should_revert() is also true for faults, so it must not override Fault).
  InvocationFrameStatusCode status_code = frame_status.value();
  if (status_code == InvocationFrameStatusCode::Return && manually_reverted) {
    status_code = InvocationFrameStatusCode::Abort;
  }
  co_return invoke_result_t{status_code, gas_consumed};
}

SystemStatus<std::unique_ptr<InvocationFrameTrace>>
SystemCallHandler::start_frame(
    Invocation invocation, InvokeFlagSet flags,
    std::span<uint32_t const> owned_auth_token_ids) noexcept
{
  assert(!last_frame_finished_);
  std::unique_ptr<InvocationFrameTrace> stashed_parent_trace = nullptr;
  if (invocation_frame_trace_ && !invocation_frame_stack_.empty()) {
    stashed_parent_trace = std::move(invocation_frame_trace_);
    try {
      invocation_frame_trace_ = std::make_unique<InvocationFrameTrace>();
    } catch (std::exception &e) {
      // Trace data is best-effort diagnostics. Under memory pressure, drop this
      // frame's trace and continue as an untraced frame rather than calling
      // panic(): start_frame() is noexcept and reachable from the test FFI mock
      // frame path, where terminating would crash an honest caller (#2473). The
      // parent trace is still restored by finish_frame() (which tolerates a
      // null child trace) once stashed_parent_trace is handed back.
      warn("dropping invocation frame trace after allocation failure: {}",
           e.what());
      invocation_frame_trace_ = nullptr;
    }
  }
  try {
    if (invocation_frame_trace_) {
      invocation_frame_trace_->invocation = invocation;
    }
  } catch (std::exception &e) {
    warn("dropping invocation frame trace after setup failure: {}", e.what());
    invocation_frame_trace_ = nullptr;
  }
  // Frame admission mutates the invocation frame stack and parent auth-token
  // ownership through throwing STL allocations. Unlike the best-effort trace
  // setup above, a failure here must not cross this noexcept boundary and
  // terminate the executor (#3058). InvocationFrameStack::start_frame() is
  // transactional: on failure it restores the state it touched and returns an
  // error. Put the parent's stashed trace back so the surviving parent frame
  // keeps its diagnostics, and report the failure as an unrecoverable error.
  //
  // The try/catch is a backstop for that transactional contract: if a future
  // admission path throws instead of returning an error (#2974), translate it
  // here rather than terminating through the noexcept boundary.
  auto admitted = [&]() -> SystemStatus<void> {
    try {
      return invocation_frame_stack_.start_frame(invocation.invoked_address,
                                                 std::move(invocation.calldata),
                                                 flags, owned_auth_token_ids);
    } catch (std::exception const &e) {
      guarded_log("frame-admission failure diagnostic", [&] {
        error("unrecoverable: frame admission threw: {}", e.what());
      });
      return SystemError{SystemErrorCode::Unrecoverable};
    } catch (...) {
      report_dropped_log("non-std exception from frame admission");
      return SystemError{SystemErrorCode::Unrecoverable};
    }
  }();
  if (!admitted.has_value()) {
    if (stashed_parent_trace) {
      invocation_frame_trace_ = std::move(stashed_parent_trace);
    }
    return admitted.error();
  }
  return stashed_parent_trace;
}

// Stays noexcept and is deliberately NOT a coroutine: creating one allocates
// its frame in the ramp function, which can throw std::bad_alloc before the
// body runs, and x23's promises define no
// get_return_object_on_allocation_failure -- a noexcept coroutine would
// std::terminate() exactly where this contract promises a structured
// Unrecoverable. The body lives in finish_frame_impl() and the failure is
// translated here, into MaybeAsync's value arm.
x23::MaybeAsync<SystemStatus<std::pair<InvokeResult, bool>>>
SystemCallHandler::finish_frame(
    InvokeStatus<void> result,
    std::unique_ptr<InvocationFrameTrace> stashed_parent_trace) noexcept
{
  try {
    return finish_frame_impl(result, std::move(stashed_parent_trace));
  } catch (std::exception const &e) {
    guarded_log("frame-completion start failure diagnostic", [&] {
      error("unrecoverable: could not start frame completion: {}", e.what());
    });
    return SystemStatus<std::pair<InvokeResult, bool>>{
        SystemError{SystemErrorCode::Unrecoverable}};
  } catch (...) {
    report_dropped_log("non-std exception starting frame completion");
    return SystemStatus<std::pair<InvokeResult, bool>>{
        SystemError{SystemErrorCode::Unrecoverable}};
  }
}

x23::Async<SystemStatus<std::pair<InvokeResult, bool>>>
SystemCallHandler::finish_frame_impl(
    InvokeStatus<void> result,
    std::unique_ptr<InvocationFrameTrace> stashed_parent_trace)
{
  assert(!invocation_frame_stack_.empty());
  assert(!last_frame_finished_);

  DbConflictResult try_invoke_conflict{DbDiagnosticLevel::kDbDiagnosticNone};
  SystemStatus<std::pair<InvokeResult, bool>> res =
      SystemError{SystemErrorCode::Unrecoverable};
  // Frame completion mutates parent-frame state through throwing STL
  // allocations: the successful-child merge (#3050, #3407), the manual-revert
  // token restore (#3406), and proposal-bit logging (#3389). Unlike frame
  // admission (#3058), completion is not transactional, so a throw can leave
  // the stack partially merged; translating it to Unrecoverable here is still
  // safe because that error ends the whole transaction, so nothing observes
  // the partial state. What it must never do is cross this noexcept boundary
  // and terminate the executor.
  try {
    if (!result.has_value()) {
      invocation_frame_stack_.set_active_frame_return(
          InvocationFrameOutput(InvocationFrameStatusCode::Fault), {});
    }

    if (invocation_frame_trace_) {
      try {
        invocation_frame_stack_.log_trace_finish(*invocation_frame_trace_);
      } catch (std::exception &e) {
        // Finish-time trace collection (collect_storage_deltas()) allocates
        // and can throw std::bad_alloc after the real storage writes already
        // succeeded; treat it as a best-effort trace drop rather than failing
        // the frame (#2646), preserving the frame cleanup and parent-trace
        // restoration below. The no-allocation reporter avoids throwing again.
        report_dropped_log(e.what());
      }
    }

    // Finish the frame on the stack first, capturing any try_invoke two-phase-
    // commit conflict. This must run after log_trace_finish() above (which
    // reads the finishing frame) and before the parent-trace restore below, so
    // the conflict can be recorded on this (still-current) child frame's
    // trace.
    [[maybe_unused]] size_t stack_size = invocation_frame_stack_.size();
    res = co_await invocation_frame_stack_.finish_frame(result,
                                                        execution_mode_data_,
                                                        &try_invoke_conflict);
    [[maybe_unused]] size_t stack_size_after = invocation_frame_stack_.size();
    assert(stack_size_after == stack_size - 1);
    if (invocation_frame_stack_.empty()) {
      last_frame_finished_ = true;
    }
  } catch (std::exception const &e) {
    guarded_log("frame-completion failure diagnostic", [&] {
      error("unrecoverable: frame completion threw: {}", e.what());
    });
    co_return SystemError{SystemErrorCode::Unrecoverable};
  } catch (...) {
    report_dropped_log("non-std exception from frame completion");
    co_return SystemError{SystemErrorCode::Unrecoverable};
  }

  // Record the conflict on this frame's trace. The default-constructed field is
  // already the kDbDiagnosticNone arm, so only a real try_invoke conflict
  // overwrites it. DbConflictResult's move-assignment is noexcept, safe in this
  // noexcept function.
  if (invocation_frame_trace_ &&
      try_invoke_conflict.level() != DbDiagnosticLevel::kDbDiagnosticNone) {
    invocation_frame_trace_->commit_conflict = std::move(try_invoke_conflict);
  }

  // Restore parent trace and attach child
  if (stashed_parent_trace) {
    auto child_trace = std::move(invocation_frame_trace_);
    invocation_frame_trace_ = std::move(stashed_parent_trace);
    // child_trace can be null when start_frame() dropped this frame's trace
    // after an allocation failure (#2473); in that case there is nothing to
    // attach and the parent's Invoke entry simply records no child trace.
    if (child_trace) {
      // Attach child_trace to parent's last syscall (the Invoke entry)
      assert(!invocation_frame_trace_->system_calls.empty());
      auto &invoke_body = invocation_frame_trace_->system_calls.back().body;
      CHECK(invoke_body.syscall_number() == SyscallNo::kInvoke);
      try {
        auto &out = invoke_body.invoke();
        out.outputs.emplace(SyscallInvokeOutputs());
        out.outputs->child_trace = std::move(*child_trace);
      } catch (std::exception &e) {
        // Child-trace attachment is best-effort diagnostics. finish_frame() is
        // noexcept and reachable from the test FFI mock-frame path, so a
        // std::bad_alloc while materializing the optional SyscallInvokeOutputs
        // must drop the optional trace rather than panic()-terminate an honest
        // caller (#2741), mirroring start_frame()'s drop-and-continue (#2473).
        // A failed emplace() leaves out.outputs empty (no partial attachment),
        // and the no-allocation reporter avoids throwing again from this catch.
        report_dropped_log(e.what());
      }
    }
  }

  co_return res;
}

void
SystemCallHandler::start_syscall_trace(SyscallNo syscall_number,
                                       GasAmount current_gas_remaining)
{
  if (!is_running()) {
    warn("start_syscall_trace: no frame running, skipping trace");
    return;
  }

  if (invocation_frame_trace_) {
    try {
      invocation_frame_trace_->system_calls.push_back(SyscallTrace{
          .gas_remaining_at_start = current_gas_remaining,
          // This field is filled in when the syscall finishes.
          .gas_remaining_at_end = std::numeric_limits<GasAmount>::max(),
          // This field is filled in by the individual syscall
          .body = SyscallTraceBody(syscall_number),
          // This field is filled in when the syscall finishes.
          .status = HostFunctionStatusXdr(HostFnErrorCode::Unrecoverable),
      });
    } catch (std::exception &e) {
      // Trace data is best-effort diagnostics. start_syscall_trace() runs from
      // wrap_after_parse() just before the noexcept syscall body, and on the
      // real RV32 path the syscall dispatches through wrap_outer() noexcept, so
      // an escaping std::bad_alloc here would terminate an honest test FFI
      // caller (#2644). Drop this frame's trace and continue untraced,
      // mirroring start_frame()'s drop-and-continue (#2473).
      // finish_syscall_trace() and get_syscall_trace_data() both tolerate a
      // null trace, so no later step touches a stale system_calls.back(). The
      // no-allocation reporter avoids throwing again from this catch.
      report_dropped_log(e.what());
      invocation_frame_trace_ = nullptr;
    }
  }
}

void
SystemCallHandler::finish_syscall_trace(HostFnStatus<SyscallErrorCode> status,
                                        GasAmount gas_remaining_at_end)
{
  if (!is_running()) {
    warn("finish_syscall_trace: no frame running, skipping trace");
    return;
  }

  if (invocation_frame_trace_) {
    CHECK(!invocation_frame_trace_->system_calls.empty());
    // field is uninitialized
    CHECK_EQ(invocation_frame_trace_->system_calls.back().gas_remaining_at_end,
             std::numeric_limits<GasAmount>::max());
    invocation_frame_trace_->system_calls.back().gas_remaining_at_end =
        gas_remaining_at_end;

    // field is uninitialized
    CHECK_EQ(invocation_frame_trace_->system_calls.back().status.error(),
             HostFnErrorCode::Unrecoverable);
    auto &status_field = invocation_frame_trace_->system_calls.back().status;
    if (status.has_value()) {
      status_field.error(HostFnErrorCode::NoneOrRecoverable);
      status_field.syscall_return_code() = status.value();
    }
    else {
      status_field.error(status.error().code());
    }
  }
}

void
SystemCallHandler::enable_tracing()
{
  // enable tracing before transaction execution starts
  assert(invocation_frame_stack_.empty());
  invocation_frame_trace_ = std::make_unique<InvocationFrameTrace>();
}

InvocationFrameTrace const *
SystemCallHandler::get_trace() const
{
  return invocation_frame_trace_.get();
}

DbConflictResult const &
SystemCallHandler::last_commit_conflict() const
{
  return db_proxy_.last_push_conflict();
}

void
SystemCallHandler::report_db_conflict_diagnostic() const
{
  auto const &conflict = db_proxy_.last_push_conflict();
  if (conflict.level() == DbDiagnosticLevel::kDbDiagnosticNone) {
    return;
  }
  trace("two-phase commit conflict diagnostics: {}", x23::show(conflict));

  // Exactly one diagnostic goes on the wire (memory-bounded end to end); under
  // kDbDiagnosticAll (debug builds) take the first. hog_db only ever sets a
  // non-None level together with a conflict, so a missing one is an invariant
  // violation, not a runtime condition -- fail fast rather than paper over it.
  DbConflictDiagnostic const *first = nullptr;
  if (conflict.level() == DbDiagnosticLevel::kDbDiagnosticFirst) {
    first = &conflict.first();
  }
  else {
    CHECK(conflict.level() == DbDiagnosticLevel::kDbDiagnosticAll);
    CHECK(!conflict.conflicts().empty());
    first = conflict.conflicts().data();
  }

  // Fire-and-forget: drive the async send in a detached coroutine so execution
  // never waits on it (the same detached-await shape as the event server's own
  // detached sends). report_tx_failure copies the client, so the send is
  // self-contained and safe to outlive us.
  const BlockHeight height = get_block_height();
  [](x23::Async<void> send)
      -> x23::Detached { co_await send; }(event_server_proxy_.report_tx_failure(
          get_source_tx_hash(),
          TxFailureDiagnostic{.attempt_height = height, .conflict = *first}));
}

void
SystemCallHandler::commit_last_call_frame_success(InvocationFrame &&frame)
{
  last_frame_.emplace(std::move(frame));
}

SystemStatus<SyscallStatus<const ReturnBuffer *>>
SystemCallHandler::get_return_buffer() const
{
  assert(!invocation_frame_stack_.empty());
  auto const &top_frame_return_output =
      invocation_frame_stack_.get_active_frame_child_return_output();
  if (!top_frame_return_output.has_value()) {
    return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeNoReturnBufferAvailable};
  }

  switch (top_frame_return_output.value().status_code()) {
  case InvocationFrameStatusCode::Return:
    return &top_frame_return_output.value().return_buffer();
  case InvocationFrameStatusCode::Abort:
    return &top_frame_return_output.value().abort_buffer();
  case InvocationFrameStatusCode::Fault:
    return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeNoReturnBufferAvailable};
  }
  error("Unrecoverable; Invalid invocation frame status code: {}",
        top_frame_return_output.value().status_code());
  return SystemError{SystemErrorCode::Unrecoverable};
}

bool
SystemCallHandler::check_auth_tokens_owned(
    std::span<uint32_t const> token_ids) const
{
  return invocation_frame_stack_.check_auth_tokens_owned(token_ids);
}

std::span<const uint32_t>
SystemCallHandler::get_returned_auth_token_ids() const
{
  assert(!invocation_frame_stack_.empty());
  return invocation_frame_stack_
      .get_active_frame_child_returned_auth_token_ids();
}

void
SystemCallHandler::set_return_buffer(
    ReturnBuffer &&returned_buffer, bool revert_frame,
    std::vector<uint32_t> returned_auth_token_ids)
{
  assert(!invocation_frame_stack_.empty());
  if (revert_frame) {
    InvocationFrameOutput output(InvocationFrameStatusCode::Abort);
    output.abort_buffer() = std::move(returned_buffer);
    invocation_frame_stack_.set_active_frame_return(std::move(output), {});
  }
  else {
    InvocationFrameOutput output(InvocationFrameStatusCode::Return);
    output.return_buffer() = std::move(returned_buffer);
    invocation_frame_stack_.set_active_frame_return(
        std::move(output), std::move(returned_auth_token_ids));
  }
}

SystemCallHandler::~SystemCallHandler()
{
  if constexpr (is_debug_build()) {
    if (!transaction_committed_or_aborted_ &&
        !std::holds_alternative<ViewingModeData>(execution_mode_data_)) {
      warn("SystemCallHandler destroyed without committing or aborting "
           "transaction");
      if (debug_force_commit_abort_) {
        panic("SystemCallHandler destroyed at invalid state: {}",
              transaction_committed_or_aborted_);
      }
    }
    if (last_frame_.has_value() && !last_frame_->extract_events().empty() &&
        root_invoke_succeeded() && !two_phase_commit_failed_) {
      panic("SystemCallHandler destroyed with events to emit");
    }
  }
}

x23::Async<SystemStatus<const PreparedContract *>>
SystemCallHandler::resolve_prepared_contract(SHA256Hash hash)
{
  // Attachments are prepared once, by perform_initial_validation.
  auto const *local = newly_created_contracts_.get_prepared_contract(hash);
  if (local != nullptr) {
    co_return local;
  }

  // Anything else is already committed, and the snapshot cache prepared it
  // eagerly when it loaded it. Borrow that image rather than building a second
  // one from the same bytes (#3536).
  auto entry = co_await snapshot_cache_.try_get_contract_entry(hash);
  if (!entry.has_value()) {
    co_return entry.error();
  }
  co_return entry.value() != nullptr ? &entry.value()->prepared : nullptr;
}

NetworkConfigParams const &
SystemCallHandler::config_params() const
{
  return snapshot_cache_.get_network_config_params();
}

x23::Async<SystemStatus<const Contract *>>
SystemCallHandler::resolve_contract(SHA256Hash const &hash) const
{
  // Check cheap in-memory map first before hitting the snapshot cache
  // (which involves a shared_mutex and potentially a database round-trip).
  auto const *local = newly_created_contracts_.get_contract_code(hash);
  if (local != nullptr) {
    co_return local;
  }

  auto entry = co_await snapshot_cache_.try_get_contract_entry(hash);
  if (!entry.has_value()) {
    co_return entry.error();
  }
  co_return entry.value() != nullptr ? &entry.value()->contract : nullptr;
}

x23::Async<SystemStatus<std::unique_ptr<Runtime>>>
SystemCallHandler::resolve_runtime(ContractAddress const &addr)
{
  // Check the snapshot for a deployed contract; on a hit, the returned entry
  // carries the back-pointer to its (eagerly translated) code.
  auto deployed_expect =
      co_await snapshot_cache_.try_get_deployed_contract(addr);
  if (!deployed_expect) {
    co_return deployed_expect.error();
  }
  const DeployedContract *deployed = *deployed_expect;
  if (deployed != nullptr) {
    co_return co_await runtime_context_.make_new_runtime(*deployed, this);
  }

  // Not in the snapshot; check the transaction's local context.
  auto local_hash =
      invocation_frame_stack_.lookup_locally_deployed_contract(addr);

  if (!local_hash && invocation_frame_stack_.empty()) {
    // Root invocation of an address with no deployed contract resolves to
    // the chain-configured default wallet, carried in the DbTip (installed
    // at genesis from GenesisConfiguration::default_wallet). The wallet's
    // auth guard rejects callers that cannot authenticate for the address.
    // If the configured hash has no contract record, resolution fails the
    // same way as any other missing contract.
    local_hash = snapshot_cache_.get_db_tip().default_wallet;
  }

  if (!local_hash) {
    co_return nullptr;
  }
  // Tx-local deployment or root default-wallet fallback: no cache entry
  // (entry stays null; the engine falls back to resolve_contract).
  co_return co_await runtime_context_.make_new_runtime(
      DeployedContract{*local_hash}, this);
}

Calldata const &
SystemCallHandler::get_calldata() const
{
  return invocation_frame_stack_.get_active_frame_calldata();
}

std::span<const uint32_t>
SystemCallHandler::get_parent_supplied_auth_tokens() const
{
  return invocation_frame_stack_
      .get_current_frame_parent_supplied_auth_tokens();
}

NondeterministicProposalData const &
SystemCallHandler::get_proposal_data() const
{
  // Either succeeded on the root tx (with or without a revert)
  // or deterministic error during execution.
  // Not allowed to have i.e. validation failure here, or block abort.
  assert(root_invoke_result_.has_readable_proposal_data());
  assert(!std::holds_alternative<ViewingModeData>(execution_mode_data_));
  if (std::holds_alternative<ProposalModeData>(execution_mode_data_)) {
    return std::get<ProposalModeData>(execution_mode_data_).proposal_data();
  }
  return std::get<ValidationModeData>(execution_mode_data_).proposal_data();
}

x23::Async<SystemStatus<void>>
SystemCallHandler::emit_events(ExecResult exec_result,
                               ProposedTransaction proposed_tx)
{
  assert(exec_result != ExecResult::ExecResultOK || root_invoke_succeeded());
  auto events = (root_invoke_succeeded() && last_frame_.has_value())
                    ? last_frame_.value().extract_events()
                    : x23::Vec<TxEmittedEvent>{};
  auto root_output =
      last_frame_.has_value()
          ? last_frame_.value().get_frame_output()
          : InvocationFrameOutput(InvocationFrameStatusCode::Fault);
  co_return co_await event_server_proxy_.emit_events(
      TxEventLog{
          .tx_hash = get_source_tx_hash(),
          .exec_result = exec_result,
          .root_output = std::move(root_output),
          .events = std::move(events),
      },
      std::move(proposed_tx));
}

SyscallStatus<void>
SystemCallHandler::log_event(Topic const *topic0, Topic const *topic1,
                             Topic const *topic2,
                             std::span<const std::byte> data)
{
  assert(!invocation_frame_stack_.empty());
  CHECK(!is_read_only());

  uint64_t serialized_size = data.size();
  auto try_add_size = [&](uint64_t delta) {
    return !integer_add_overflow(serialized_size, delta, &serialized_size);
  };
  if ((topic0 && !try_add_size(topic0->size())) ||
      (topic1 && !try_add_size(topic1->size())) ||
      (topic2 && !try_add_size(topic2->size()))) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  if (serialized_size > kMaxEventLength) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  x23::Buf buf;
  uint32_t num_topics = 0;
  if (topic0) {
    buf.append_range(*topic0);
    num_topics++;
  }
  if (topic1) {
    buf.append_range(*topic1);
    num_topics++;
  }
  if (topic2) {
    buf.append_range(*topic2);
    num_topics++;
  }

  buf.append_range(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(data.data()),
                               data.size()));

  return log_event(num_topics, std::as_bytes(std::span(buf)));
}

SyscallStatus<void>
SystemCallHandler::log_event(uint32_t num_topics,
                             std::span<const std::byte> data)
{
  assert(!invocation_frame_stack_.empty());
  CHECK(!is_read_only());
  if (num_topics > kMaxTopics) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  if (data.size() > kMaxEventLength) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  if (invocation_frame_stack_.event_count() >= kMaxEventsPerTransaction) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  auto message = make_tx_emitted_event(get_current_address(), num_topics, data);
  invocation_frame_stack_.log_event(std::move(message));
  return {};
}

ContractAddress
SystemCallHandler::static_get_deploy_address(ContractAddress const &sender,
                                             SHA256Hash const &nonce,
                                             SHA256Hash const &contract_hash)
{
  return sha256(ContractDeploymentHashPreimage{.deployer_address = sender,
                                               .nonce = nonce,
                                               .contract_hash = contract_hash});
}

namespace {
constexpr bool
is_banned_redeploy_native_contract(
    HogexecPrecompiledNativeContracts native_contract_id)
{
  switch (native_contract_id) {
  case HogexecPrecompiledNativeContracts::kNativeContractHogToken:
  case HogexecPrecompiledNativeContracts::kNativeContractDefaultWallet:
    return true;
  }
  panic("Unknown native contract deployed: {}", native_contract_id);
}
} // namespace

x23::Async<SystemStatus<SyscallStatus<ContractAddress>>>
SystemCallHandler::deploy_contract(SHA256Hash const &nonce,
                                   SHA256Hash const &contract_hash)
{
  assert(!invocation_frame_stack_.empty());
  CHECK(!is_read_only());

  // check that contract_hash actually exists
  // This causes a load of the contract to the cache,
  // but this is in most cases no-cost, as a deployed contract will _probably_
  // be immediately invoked with e.g. a constructor
  auto contract_res = co_await resolve_contract(contract_hash);
  if (!contract_res.has_value()) {
    co_return contract_res.error();
  }
  if (contract_res.value() == nullptr) {
    co_return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  assert(contract_res.value() != nullptr);
  auto const &contract = *contract_res.value();

  // Native (precompiled) contracts cannot be deployed - they are set up at
  // genesis and should not be redeployed by user transactions
  if (contract.contract_type() == ContractType::kNative &&
      is_banned_redeploy_native_contract(contract.native_contract_id())) {
    co_return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  auto deploy_addr =
      static_get_deploy_address(get_current_address(), nonce, contract_hash);

  auto deploy_check_status =
      co_await snapshot_cache_.try_get_deployed_contract(deploy_addr);
  if (!deploy_check_status) {
    co_return deploy_check_status.error();
  }
  if (*deploy_check_status != nullptr) {
    co_return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeContractAlreadyDeployed};
  }

  if (invocation_frame_stack_.lookup_locally_deployed_contract(deploy_addr)
          .has_value()) {
    // contract has already been deployed in this transaction
    co_return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeContractAlreadyDeployed};
  }

  auto contract_size = static_cast<uint32_t>(contract_blob_size(contract));

  // deploy new contract
  invocation_frame_stack_.deploy_contract(
      deploy_addr,
      DeployedContractHash{.hash = contract_hash,
                           .contract_size = contract_size,
                           .contract_type = contract.contract_type()});

  co_return deploy_addr;
}

x23::Async<SystemStatus<StorageRead<const Bytestring *>>>
SystemCallHandler::get_bytestring(StorageKeyContractData const &storage_key)
{
  using Read = StorageRead<const Bytestring *>;

  const FrameItemKey item_key =
      invocation_frame_stack_.frame_item_key(storage_key);
  auto *item_view = invocation_frame_stack_.get_item_view(item_key);
  assert(item_view);

  // Everything up to the load below answers out of the frame stack, so it
  // reaches no database and costs no load gas.
  if (auto const *bytestring_item = item_view->get<BytestringItem>()) {
    co_return Read{&bytestring_item->bytestring_, 0};
  }

  if (item_view->get<DeletedItem>()) {
    co_return Read{SyscallError{
                       SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist},
                   0};
  }

  if (auto const *unmodified_item = item_view->get<UnmodifiedItem>()) {
    // Prices the fetch ensure_loaded() is about to do. Both turn on the same
    // is_loaded() predicate, and ensure_loaded() stays a non-coroutine so a
    // hit still allocates no frame.
    const GasAmount load_gas =
        unmodified_item->is_loaded() ? 0 : note_item_load(item_key);
    if (auto loaded =
            co_await ensure_loaded(*unmodified_item, snapshot_cache_, item_key);
        !loaded.has_value()) {
      co_return loaded.error();
    }
    auto const &base_item = unmodified_item->base_item();
    auto type = base_item.type();
    if (!type) {
      co_return Read{SyscallError{
                         SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist},
                     load_gas};
    }
    if (*type != ItemViewType::kBytestring) {
      co_return Read{
          SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput},
          load_gas};
    }
    co_return Read{&base_item.bytestring(), load_gas};
  }

  co_return Read{SyscallError{
                     SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput},
                 0};
}

x23::Async<SystemStatus<SyscallStatus<void>>>
SystemCallHandler::set_bytestring(StorageKeyContractData const &storage_key,
                                  std::span<const std::byte> input_bytestring,
                                  StorageDeltaFlagSet input_flags)
{
  CHECK(!is_read_only());

  if (!SystemCallHandler::validate_flags<StorageDeltaFlags::ExclusiveLock>(
          input_flags)) {
    co_return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  if (input_bytestring.size() > config_params().max_bytestring_len) {
    co_return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);

  Bytestring new_string;
  new_string.resize_uninitialized(input_bytestring.size());
  if (!input_bytestring.empty()) {
    assert(new_string.data() != nullptr);
    assert(input_bytestring.data() != nullptr);
    std::memcpy(new_string.data(),
                reinterpret_cast<const uint8_t *>(input_bytestring.data()),
                input_bytestring.size());
  }

  co_return item_view->set(BytestringItem(std::move(new_string)), input_flags);
}

GasAmount
SystemCallHandler::note_item_load(FrameItemKey const &key)
{
  return loaded_keys_.insert(key).second ? kDBRPCCost : 0;
}

GasAmount
SystemCallHandler::note_contract_load(SHA256Hash const &contract_hash)
{
  // Tx-local hashes need no snapshot lookup, so they are free.
  if (newly_created_contracts_.get_contract_code(contract_hash) != nullptr) {
    return 0;
  }
  return loaded_contract_hashes_.insert(contract_hash).second ? kDBRPCCost : 0;
}

x23::Async<
    SystemStatus<StorageRead<std::pair<std::optional<int128_t>, int128_t>>>>
SystemCallHandler::get_nnint(StorageKeyContractData const &storage_key,
                             bool load_from_snapshot)
{
  using Value = std::pair<std::optional<int128_t>, int128_t>;
  using Read = StorageRead<Value>;

  const FrameItemKey item_key =
      invocation_frame_stack_.frame_item_key(storage_key);
  auto *item_view = invocation_frame_stack_.get_item_view(item_key);
  assert(item_view);

  // A branch that answers out of the frame stack reaches no database, so it
  // costs no load gas; only the two that read the snapshot below can.
  if (auto const *nnint_item = item_view->get<NNIntItem>()) {
    if (nnint_item->base_value_.has_value()) {
      co_return Read{Value{from_xdr(*nnint_item->base_value_),
                           from_xdr(nnint_item->add_value_)},
                     0};
    }
    // If we're not loading from the snapshot, we can return nullopt for the
    // base value.
    if (!load_from_snapshot) {
      co_return Read{Value{std::nullopt, from_xdr(nnint_item->add_value_)}, 0};
    }

    // Reaches the database the same way UnmodifiedItem::load() does, so it
    // is billed the same way: a staged add with no set value followed by a
    // forced get lands here rather than in the branch below.
    const GasAmount load_gas = note_item_load(item_key);
    auto snapshot_value =
        co_await snapshot_cache_.try_get_snapshot_item(item_key);
    if (!snapshot_value.has_value()) {
      co_return snapshot_value.error();
    }

    // In this case, we're returning the parameters of the prior NNInt write.
    // A bit of a weird case, because we're following an NNintSet(nullopt, add)
    // with a Get(force_load_from_snapshot=true).  We augment the prior NNint
    // write with what the set value will be, when the write is applied to the
    // database. Hence we default to 0 if the snapshot value is not an NNInt.
    auto snapshot_type = snapshot_value.value()->type();
    if (!snapshot_type || snapshot_type.value() != ItemViewType::kNNInt) {
      co_return Read{Value{0, from_xdr(nnint_item->add_value_)}, load_gas};
    }
    co_return Read{Value{snapshot_value.value()->nnint(),
                         from_xdr(nnint_item->add_value_)},
                   load_gas};
  }

  if (item_view->get<DeletedItem>()) {
    if (!load_from_snapshot) {
      co_return Read{Value{std::nullopt, 0}, 0};
    }
    co_return Read{SyscallError{
                       SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist},
                   0};
  }

  if (auto const *unmodified_item = item_view->get<UnmodifiedItem>()) {
    if (!load_from_snapshot) {
      co_return Read{Value{std::nullopt, 0}, 0};
    }

    // Prices the fetch ensure_loaded() is about to do. Both turn on the same
    // is_loaded() predicate, and ensure_loaded() stays a non-coroutine so a
    // hit still allocates no frame.
    const GasAmount load_gas =
        unmodified_item->is_loaded() ? 0 : note_item_load(item_key);
    if (auto loaded =
            co_await ensure_loaded(*unmodified_item, snapshot_cache_, item_key);
        !loaded.has_value()) {
      co_return loaded.error();
    }
    auto const &base_item = unmodified_item->base_item();
    auto type = base_item.type();
    if (!type) {
      assert(load_from_snapshot);
      // We are in load_from_snapshot mode, so we return KeyNotExist instead of
      // std::nullopt, mirroring the behavior above of the DeletedItem case.
      co_return Read{SyscallError{
                         SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist},
                     load_gas};
    }
    if (*type != ItemViewType::kNNInt) {
      co_return Read{
          SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput},
          load_gas};
    }
    co_return Read{Value{base_item.nnint(), 0}, load_gas};
  }

  // the value written at this key is not an NNInt, so we return an
  // InvalidSyscallInput error. This mirrors the case above, where the snapshot
  // *type != kNNInt.
  if (!load_from_snapshot) {
    co_return Read{Value{std::nullopt, 0}, 0};
  }
  co_return Read{SyscallError{
                     SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput},
                 0};
}

x23::Async<SystemStatus<SyscallStatus<void>>>
SystemCallHandler::set_nnint(StorageKeyContractData const &storage_key,
                             std::optional<int128_t> set_value,
                             const int128_t &add_value,
                             StorageDeltaFlagSet input_flags)
{
  CHECK(!is_read_only());

  if (!SystemCallHandler::validate_flags<StorageDeltaFlags::ExclusiveLock>(
          input_flags)) {
    co_return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);

  // Writing to an nnint, without explicitly defining the set value,
  // is defined as equivalent to outputting set_add with set equal to
  // whatever the current database snapshot value is.  As such, it is correct to
  // overwrite the set value here with nullopt, in such a case.
  co_return item_view->set(NNIntItem{.base_value_ =
                                         set_value.has_value()
                                             ? std::optional<int128_xdr>(
                                                   to_xdr(*set_value))
                                             : std::nullopt,
                                     .add_value_ = to_xdr(add_value)},
                           input_flags);
}

x23::Async<SystemStatus<SyscallStatus<void>>>
SystemCallHandler::delete_object(StorageKeyContractData const &storage_key,
                                 StorageDeltaFlagSet flags)
{
  CHECK(!is_read_only());

  if (!SystemCallHandler::validate_flags<StorageDeltaFlags::ExclusiveLock>(
          flags)) {
    co_return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);
  co_return item_view->set(DeletedItem{}, flags);
}

namespace {
auto
queue_entry_from_span(std::span<const std::byte> span)
{
  QueueEntry new_entry;
  new_entry.resize_uninitialized(span.size());
  if (!span.empty()) {
    assert(new_entry.data() != nullptr);
    assert(span.data() != nullptr);
    std::memcpy(new_entry.data(),
                reinterpret_cast<const uint8_t *>(span.data()), span.size());
  }
  return new_entry;
}

x23::Async<x23::Vec<QueueEntry>>
get_retrocausally_consumed_entries(ExecutionModeData &execution_mode_data,
                                   SnapshotStorageItem const &base_item,
                                   AddressAndKey const &addrkey, uint32_t count)
{
  if (auto *vd = std::get_if<ValidationModeData>(&execution_mode_data)) {
    co_return vd->get_observed_queue_entries(addrkey, count);
  }
  if (auto *vwd = std::get_if<ViewingModeData>(&execution_mode_data)) {
    co_return vwd->get_observed_queue_entries(addrkey, count);
  }
  co_return co_await base_item.get_retrocausally_consumed_entries(count);
}

// An RCQ access to a DeletedItem key resolves against a fully-consumed view
// of the snapshot's queue: the tx-local delete hides every entry, but the
// retrocausally-consumed count already recorded on the view must be carried
// forward. Loads the snapshot base item fresh (the DeletedItem holds none)
// and builds that view.
x23::Async<SystemStatus<RetrocausalQueueItem>>
make_deleted_key_queue_item(FrameItemKey const &item_key,
                            AddressAndKey const &addrkey,
                            uint32_t retrocausally_consumed_count,
                            StorageSnapshotCache &snapshot_cache,
                            ExecutionModeData &execution_mode_data)
{
  UnmodifiedItem unmodified_item;
  auto load_status = co_await unmodified_item.load(snapshot_cache, item_key);
  if (!load_status.has_value()) {
    co_return load_status.error();
  }

  auto consumed =
      co_await get_retrocausally_consumed_entries(execution_mode_data,
                                                  unmodified_item.base_item(),
                                                  addrkey,
                                                  retrocausally_consumed_count);
  co_return RetrocausalQueueItem::make_fully_consumed_item(
      unmodified_item.base_item(), std::move(consumed));
}

x23::Async<SystemStatus<SyscallStatus<RetrocausalQueueItem *>>>
retrocausal_queue_item_from_view(ItemView *item_view,
                                 FrameItemKey const &item_key,
                                 AddressAndKey const &addrkey,
                                 StorageSnapshotCache &snapshot_cache,
                                 ExecutionModeData &execution_mode_data)
{
  if (auto *queue_item = item_view->get<RetrocausalQueueItem>()) {
    co_return queue_item;
  }
  if (auto const *unmodified_item = item_view->get<UnmodifiedItem>()) {
    if (auto loaded =
            co_await ensure_loaded(*unmodified_item, snapshot_cache, item_key);
        !loaded.has_value()) {
      co_return loaded.error();
    }
    RetrocausalQueueItem new_item(unmodified_item->base_item());

    auto set_status = item_view->set(new_item);
    if (!set_status.has_value()) {
      co_return set_status.error();
    }
    co_return item_view->get<RetrocausalQueueItem>();
  }

  if (item_view->get<DeletedItem>()) {
    auto new_item = co_await make_deleted_key_queue_item(
        item_key, addrkey, item_view->get_retrocausally_consumed_count(),
        snapshot_cache, execution_mode_data);
    if (!new_item.has_value()) {
      co_return new_item.error();
    }

    auto set_status = item_view->set(new_item.value());
    if (!set_status.has_value()) {
      co_return set_status.error();
    }
    co_return item_view->get<RetrocausalQueueItem>();
  }

  co_return SyscallError{
      SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
}

x23::Async<SystemStatus<SyscallStatus<QueueEntry>>>
load_next_queue_entry(RetrocausalQueueItem &queue_item,
                      ExecutionModeData &execution_mode_data,
                      bool advance_cursor)
{
  auto entry =
      co_await queue_item.get_next_item(execution_mode_data, advance_cursor);
  if (!entry.has_value()) {
    co_return entry.error();
  }

  // Return whatever get_next_item resolved. Only a concurrent read consumes an
  // observation slot; a self or past-block consume records no observation and
  // so is never subject to the per-tx observation cap. get_next_item already
  // returns nullptr for a concurrent read the cap blocked, so a non-null entry
  // here is always a legitimate read -- return it even once the cap is exceeded
  // rather than dropping it behind a read-limit error (#3611). The pointer
  // aliases into owned storage and is copied here into the syscall result.
  if (const QueueEntry *value = entry.value()) {
    co_return *value;
  }

  // No entry resolved. If that is because a concurrent read hit the per-tx
  // observation cap, surface the deterministic read-limit error so a contract
  // can't drive the bounded observation vector past kMaxConcurrentRcqReadsPerTx
  // (#2894). This is a dedicated resource-limit code contracts can handle as a
  // stop condition, not a malformed-input error.
  if (observation_cap_exceeded(execution_mode_data)) {
    co_return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeRetrocausalQueueReadLimitReached};
  }

  co_return SyscallError{
      SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry};
}

x23::Async<SystemStatus<SyscallStatus<QueueEntry>>>
retrocausal_queue_peek_from_view(ItemView *item_view,
                                 FrameItemKey const &item_key,
                                 AddressAndKey const &addrkey,
                                 StorageSnapshotCache &snapshot_cache,
                                 ExecutionModeData &execution_mode_data)
{
  if (auto *queue_item = item_view->get<RetrocausalQueueItem>()) {
    co_return co_await load_next_queue_entry(*queue_item, execution_mode_data,
                                             /*advance_cursor=*/false);
  }

  if (auto const *unmodified_item = item_view->get<UnmodifiedItem>()) {
    if (auto loaded =
            co_await ensure_loaded(*unmodified_item, snapshot_cache, item_key);
        !loaded.has_value()) {
      co_return loaded.error();
    }

    RetrocausalQueueItem queue_item(unmodified_item->base_item());
    co_return co_await load_next_queue_entry(queue_item, execution_mode_data,
                                             /*advance_cursor=*/false);
  }

  if (item_view->get<DeletedItem>()) {
    auto queue_item = co_await make_deleted_key_queue_item(
        item_key, addrkey, item_view->get_retrocausally_consumed_count(),
        snapshot_cache, execution_mode_data);
    if (!queue_item.has_value()) {
      co_return queue_item.error();
    }
    co_return co_await load_next_queue_entry(queue_item.value(),
                                             execution_mode_data,
                                             /*advance_cursor=*/false);
  }

  co_return SyscallError{
      SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
}
} // namespace

x23::Async<SystemStatus<SyscallStatus<void>>>
SystemCallHandler::retrocausal_queue_insert(
    StorageKeyContractData const &key, std::span<const std::byte> queue_entry)
{
  if (queue_entry.size() > kQueueEntryMaxLen) {
    co_return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  const FrameItemKey item_key = invocation_frame_stack_.frame_item_key(key);
  auto addrkey = invocation_frame_stack_.get_addrkey(key);
  auto *item_view = invocation_frame_stack_.get_item_view(item_key);
  assert(item_view);

  auto queue_item =
      co_await retrocausal_queue_item_from_view(item_view, item_key, addrkey,
                                                snapshot_cache_,
                                                execution_mode_data_);
  if (!queue_item.has_value()) {
    co_return queue_item.error();
  }
  if (!queue_item.value().has_value()) {
    co_return queue_item.value().error();
  }

  // Now we have either grabbed the preexisting RCQItem, or created a new one.
  // In either case, we can now insert the new entry.
  co_return queue_item.value().value()->insert_entry(
      queue_entry_from_span(queue_entry));
}

x23::Async<SystemStatus<SyscallStatus<QueueEntry>>>
SystemCallHandler::retrocausal_queue_consume(StorageKeyContractData const &key,
                                             bool peek)
{
  const FrameItemKey item_key = invocation_frame_stack_.frame_item_key(key);
  auto addrkey = invocation_frame_stack_.get_addrkey(key);
  auto *item_view = invocation_frame_stack_.get_item_view(item_key);
  assert(item_view);

  if (peek) {
    co_return co_await retrocausal_queue_peek_from_view(item_view, item_key,
                                                        addrkey,
                                                        snapshot_cache_,
                                                        execution_mode_data_);
  }

  // For unmodified keys, probe with a local item first. Only persist
  // into the view if an entry is found, so that a no-op consume on an
  // absent key does not pin the storage type.
  if (auto const *unmodified_item = item_view->get<UnmodifiedItem>()) {
    if (auto loaded =
            co_await ensure_loaded(*unmodified_item, snapshot_cache_, item_key);
        !loaded.has_value()) {
      co_return loaded.error();
    }
    RetrocausalQueueItem queue_item(unmodified_item->base_item());
    auto entry =
        co_await load_next_queue_entry(queue_item, execution_mode_data_,
                                       /*advance_cursor=*/true);
    if (entry.has_value() && entry.value().has_value()) {
      auto set_status = item_view->set(queue_item);
      if (!set_status.has_value()) {
        co_return set_status.error();
      }
      item_view->sync_retrocausally_consumed(queue_item);
    }
    co_return entry;
  }

  if (auto *queue_item = item_view->get<RetrocausalQueueItem>()) {
    auto entry =
        co_await load_next_queue_entry(*queue_item, execution_mode_data_,
                                       /*advance_cursor=*/true);
    item_view->sync_retrocausally_consumed(*queue_item);
    co_return entry;
  }

  // Same treatment for deleted keys: a no-op consume should leave the
  // key deleted rather than converting it to an empty RCQ.
  if (item_view->get<DeletedItem>()) {
    auto queue_item_res = co_await make_deleted_key_queue_item(
        item_key, addrkey, item_view->get_retrocausally_consumed_count(),
        snapshot_cache_, execution_mode_data_);
    if (!queue_item_res.has_value()) {
      co_return queue_item_res.error();
    }
    auto &queue_item = queue_item_res.value();
    auto entry =
        co_await load_next_queue_entry(queue_item, execution_mode_data_,
                                       /*advance_cursor=*/true);
    if (entry.has_value() && entry.value().has_value()) {
      auto set_status = item_view->set(queue_item);
      if (!set_status.has_value()) {
        co_return set_status.error();
      }
      item_view->sync_retrocausally_consumed(queue_item);
    }
    co_return entry;
  }

  co_return SyscallError{
      SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
}

// == Native token syscalls ==

x23::Async<SystemStatus<StorageRead<int128_t>>>
SystemCallHandler::get_native_token_balance(
    ContractAddress const &contract_address)
{
  using Read = StorageRead<int128_t>;
  // Set by whichever of the two snapshot reads below runs, if either does.
  GasAmount load_gas = 0;

  AddressAndKey addrkey{.address = contract_address,
                        .key =
                            StorageKey(StorageKeyType::FreeNativeTokenBalance)};
  const FrameItemKey item_key = FrameItemKey::from(addrkey);

  auto *item_view = invocation_frame_stack_.get_item_view(item_key);
  assert(item_view);

  if (auto *unmodified_item = item_view->get<UnmodifiedItem>()) {
    // Prices the fetch ensure_loaded() is about to do; see get_bytestring().
    load_gas = unmodified_item->is_loaded() ? 0 : note_item_load(item_key);
    if (auto loaded =
            co_await ensure_loaded(*unmodified_item, snapshot_cache_, item_key);
        !loaded.has_value()) {
      co_return loaded.error();
    }
    auto const &base_item = unmodified_item->base_item();
    assert(base_item.type() == ItemViewType::kNativeTokenBalance);
    // Native balances are nonnegative by construction: the commit path's
    // bounds locks (NNIntSetAddArgs::try_acquire_bounds_lock) refuse any
    // reservation that would drive a nonnegative base below zero. A negative
    // committed row can therefore only come from on-disk corruption, so
    // fail-stop rather than let get_balance_of/get_balance convert it to
    // uint128_t and hand a contract UINT128_MAX. CHECK, not assert: the
    // conversion is unguarded exactly where it matters, in NDEBUG builds.
    CHECK(base_item.native_token_balance() >= 0);
    co_return Read{base_item.native_token_balance(), load_gas};
  }

  auto const *balance_item = item_view->get<NativeTokenBalanceItem>();
  assert(balance_item);

  // Resolve the base balance: the tx-local written value when there is one,
  // otherwise the snapshot's. An absent or untyped snapshot item is a zero
  // balance. (This was an immediately-invoked lambda; it is inlined here
  // because the snapshot read suspends and a lambda cannot be a coroutine
  // returning a plain SystemStatus.)
  int128_t base = 0;
  if (balance_item->base_value_.has_value()) {
    base = from_xdr(*balance_item->base_value_);
  }
  else {
    // Same database read as the branch above, so it is billed the same way.
    load_gas = note_item_load(item_key);
    auto snapshot_value =
        co_await snapshot_cache_.try_get_snapshot_item(item_key);
    if (!snapshot_value.has_value()) {
      co_return snapshot_value.error();
    }
    auto snapshot_type = snapshot_value.value()->type();
    if (snapshot_type.has_value()) {
      assert(snapshot_type.value() == ItemViewType::kNativeTokenBalance);
      base = snapshot_value.value()->native_token_balance();
    }
  }

  if (integer_add_overflow(base, from_xdr(balance_item->add_value_))) {
    co_return Read{SyscallError{
                       SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput},
                   load_gas};
  }

  int128_t balance = base + from_xdr(balance_item->add_value_);
  if (balance < 0) {
    co_return Read{SyscallError{
                       SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput},
                   load_gas};
  }
  co_return Read{balance, load_gas};
}

x23::Async<SystemStatus<SyscallStatus<void>>>
SystemCallHandler::modify_native_token_balance(
    ContractAddress const &contract_address, int128_t transfer_amount)
{
  assert(!invocation_frame_stack_.empty());
  if (is_read_only()) {
    co_return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  AddressAndKey addrkey{.address = contract_address,
                        .key =
                            StorageKey(StorageKeyType::FreeNativeTokenBalance)};

  auto *item_view = invocation_frame_stack_.get_item_view(addrkey);
  assert(item_view);

  if (auto const *balance_item = item_view->get<NativeTokenBalanceItem>()) {
    auto current_add = from_xdr(balance_item->add_value_);
    if (integer_add_overflow(current_add, transfer_amount)) {
      co_return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    }
    auto new_add = current_add + transfer_amount;
    co_return item_view->set(NativeTokenBalanceItem(
        NNIntItem{.base_value_ = balance_item->base_value_,
                  .add_value_ = to_xdr(new_add)}));
  }

  assert(item_view->get<UnmodifiedItem>());
  co_return item_view->set(
      NativeTokenBalanceItem(NNIntItem{.base_value_ = std::nullopt,
                                       .add_value_ = to_xdr(transfer_amount)}));
}

// == Authentication syscalls ==

namespace {
AddressAndKey
auth_data_addrkey(ContractAddress const &address)
{
  return {.address = address, .key = StorageKey(StorageKeyType::AuthData)};
}

// Apply a mutation to the given address's tx-local AddressAuthDataItem,
// creating the item from scratch on first touch. Synchronous: the item view
// is tx-local state, so no snapshot read is needed. The mutation runs exactly
// once per call.
SystemStatus<SyscallStatus<void>>
modify_auth_data(InvocationFrameStack &invocation_frame_stack,
                 ContractAddress const &address, auto mutate)
{
  auto *item_view =
      invocation_frame_stack.get_item_view(auth_data_addrkey(address));
  assert(item_view);

  if (auto *address_auth_data_item = item_view->get<AddressAuthDataItem>()) {
    mutate(*address_auth_data_item);
    return SystemStatus<SyscallStatus<void>>{};
  }

  if (item_view->get<DeletedItem>()) [[unlikely]] {
    error("Unrecoverable: unexpected manual delete of authentication data");
    return SystemError{SystemErrorCode::Unrecoverable};
  }

  assert(item_view->get<UnmodifiedItem>());
  AddressAuthDataItem new_item;
  mutate(new_item);
  return item_view->set(new_item);
}
} // namespace

x23::Async<SystemStatus<SyscallStatus<void>>>
SystemCallHandler::change_public_key_registration(uint32_t key_index,
                                                  PublicKeyConfigOpt new_key)
{
  if (key_index >= kMaxSigners) {
    co_return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  assert(!invocation_frame_stack_.empty());
  co_return modify_auth_data(invocation_frame_stack_, get_current_address(),
                             [&](AddressAuthDataItem &item) {
                               item.change_public_key(key_index,
                                                      std::move(new_key));
                             });
}

x23::Async<SystemStatus<SyscallStatus<void>>>
SystemCallHandler::set_address_required_weight(Weight required_weight)
{
  co_return modify_auth_data(invocation_frame_stack_, get_current_address(),
                             [&](AddressAuthDataItem &item) {
                               item.set_required_weight(required_weight);
                             });
}

const AuthClaim &
SystemCallHandler::get_auth_claim() const
{
  return auth_claim_;
}

// == Hash buffer syscalls ==

SyscallStatus<void>
SystemCallHandler::sha256_update(uint32_t input_index,
                                 std::span<const std::byte> input_buffer,
                                 bool input_reset)
{
  assert(!invocation_frame_stack_.empty());
  return invocation_frame_stack_.get_active_frame_hash_buffers().update(
      input_index, input_buffer, input_reset);
}

SyscallStatus<void>
SystemCallHandler::sha256_copy(uint32_t input_src_index,
                               uint32_t input_dst_index)
{
  assert(!invocation_frame_stack_.empty());
  return invocation_frame_stack_.get_active_frame_hash_buffers().copy(
      input_src_index, input_dst_index);
}

SyscallStatus<SHA256Hash>
SystemCallHandler::sha256_finalize(uint32_t input_index)
{
  assert(!invocation_frame_stack_.empty());
  size_t output_size = 0;
  auto r = invocation_frame_stack_.get_active_frame_hash_buffers().finalize(
      input_index, &output_size);
  if (auto *stats = get_transaction_stats()) {
    stats->sha256_total_bytes_processed += output_size;
  }
  return r;
}

// == Block statistics syscalls ==

BlockHeight
SystemCallHandler::get_block_height() const
{
  // The contract-visible current block height is the *executing* block height,
  // which is the parent tip's height + 1: HogDb opens the BlockTX at
  // get_tip().value.proposal_header.block_height + 1 (see
  // RPCContext::open_block_tx_locked) and get_db_tip() returns the parent tip
  // for an open block. Contract-facing height-gated logic (auth-token expiry,
  // timelocks) must observe the executing height, matching the replay-cache
  // expiry check.
  return snapshot_cache_.get_db_tip().value.proposal_header.block_height + 1;
}

NetworkHash const &
SystemCallHandler::get_network_hash() const
{
  // Identical in every header, so the parent tip's copy is already this
  // block's value, no adjustment needed.
  return snapshot_cache_.get_db_tip().value.proposal_header.network_hash;
}

TimePoint
SystemCallHandler::get_block_timepoint() const
{
  // Not derivable from the tip: for an open block that is the parent, whose
  // timepoint is the previous block's.
  return snapshot_cache_.block_execution_context().block_timepoint;
}

// == Auth token syscalls ==

SyscallStatus<uint32_t>
SystemCallHandler::mint_auth_token(AuthToken const &auth_token)
{
  return invocation_frame_stack_.mint_auth_token(auth_token);
}

SyscallStatus<InvocationFrameStack::ConsumeAuthTokenResult>
SystemCallHandler::consume_auth_token(uint32_t token_id,
                                      BlockHeight current_height)
{
  auto const *token = invocation_frame_stack_.lookup_auth_token(token_id);
  if (token == nullptr) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  if (token->expiry_height < current_height) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  return invocation_frame_stack_.consume_auth_token(token_id);
}

} // namespace hogd
