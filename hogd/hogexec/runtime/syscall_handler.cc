#include "hogd/hogexec/runtime/syscall_handler.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "auth_token.hh"
#include "contract.hh"
#include "event.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/event/utilities.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/borrowed_contract_batch.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/invocation_frame.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/item/item_view.h"
#include "hogd/hogexec/runtime/item/item_view_type.h"
#include "hogd/hogexec/runtime/item/snapshot_storage_item.h"
#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/metrics.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/storage_item/replay_cache.h"
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

// TODO(geoff): Setup configuration parameters for alternative wasm engines
// and/or shared contexts.
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
        db_proxy_),
    source_tx_hash_(sha256(tx)), auth_claim_(tx.auth_claim),
    root_invocation_(tx.invocation), tx_uniquifier_(tx.uniquifier),
    root_invoked_address_(tx.invocation.invoked_address),
    newly_created_contracts_([&] {
      std::unordered_map<SHA256Hash, std::unique_ptr<const Contract>,
                         ByteArrayHasher>
          contracts;
      contracts.reserve(tx.contracts_to_deploy.size());

      for (auto const &contract : tx.contracts_to_deploy) {
        contracts.emplace(sha256(contract),
                          std::make_unique<const Contract>(contract));
      }
      return contracts;
    }()),
    execution_mode_data_(
        [](ExecutionModeCtor execution_mode_ctor) -> ExecutionModeData {
          if (std::holds_alternative<ProposalModeCtor>(execution_mode_ctor)) {
            return ProposalModeData{};
          }
          assert(!std::holds_alternative<ViewingModeCtor>(execution_mode_ctor));
          return ExecutionModeData{std::in_place_type<ValidationModeData>,
                                   std::get<ValidationModeCtor>(
                                       execution_mode_ctor)};
        }(std::move(execution_mode_ctor))),
    snapshot_cache_(snapshot_cache)
{}

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
        db_proxy_),
    source_tx_hash_(source_tx_hash), auth_claim_(std::move(auth_claim)),
    newly_created_contracts_(),
    execution_mode_data_(
        [](ExecutionModeCtor execution_mode_ctor) -> ExecutionModeData {
          if (std::holds_alternative<ProposalModeCtor>(execution_mode_ctor)) {
            return ProposalModeData{};
          }
          if (std::holds_alternative<ViewingModeCtor>(execution_mode_ctor)) {
            return ExecutionModeData{std::in_place_type<ViewingModeData>,
                                     std::get<ViewingModeCtor>(
                                         execution_mode_ctor)};
          }
          return ExecutionModeData{std::in_place_type<ValidationModeData>,
                                   std::get<ValidationModeCtor>(
                                       execution_mode_ctor)};
        }(std::move(execution_mode_ctor))),
    snapshot_cache_(snapshot_cache)
{}

void
SystemCallHandler::reject_if_replay_uniquifier_expired()
{
  // Best-effort early gate (#2568): a transaction whose replay uniquifier has
  // already expired is rejected here, before root_invoke() can run the contract
  // body. The HogDB replay cache remains the authoritative atomic consume at
  // commit time; this only stops the body from running against an input that
  // final commit will reject anyway. The executor calls this on a freshly
  // opened (current) block; it is intentionally not folded into root_invoke()
  // so the low-level handler primitive keeps its existing contract for callers
  // that drive it directly.
  assert(cumulative_invoke_result_.is_root_success());

  // Mirror add_replay_nonce_delta's guards: only real transactions carry a
  // uniquifier, and the initial-token-claim precompile bypasses the replay
  // cache entirely, so neither consumes -- nor needs an expiry gate for -- a
  // uniquifier.
  if (!tx_uniquifier_.has_value()) {
    return;
  }
  assert(root_invoked_address_.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  if (root_invoked_address_.value() == kInitialTokenClaimPrecompileAddress) {
    return;
  }

  auto height = get_block_height();
  if (!height.has_value()) {
    // Can't read the db tip to determine the execution height, so we can't
    // positively conclude the uniquifier is expired. Skip the early gate and
    // let normal execution proceed; the authoritative replay-cache reservation
    // in try_commit_transaction still rejects an expired uniquifier (just
    // later). This keeps the gate purely best-effort -- it only short-circuits
    // when it can prove expiry.
    return;
  }

  // ReplayCacheLock rejects a uniquifier whose block_expiry_height is strictly
  // less than the *executing* block's height. HogDB's BlockTx executes at
  // parent_tip.height + 1 (see RPCContext::open_block_tx_locked), while
  // get_block_height() reads that parent tip, so the executing height is +1.
  // Matching that comparison here rejects exactly the uniquifiers the late
  // replay-cache reservation would reject -- only earlier.
  BlockHeight executing_height = height.value() + 1;
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  if (tx_uniquifier_.value().block_expiry_height < executing_height) {
    cumulative_invoke_result_.result_ =
        InvokeError{InvokeErrorCode::ValidationFailure};
  }
}

void
SystemCallHandler::perform_initial_validation()
{
  // An earlier gate may have already rejected the transaction (e.g.
  // reject_if_replay_uniquifier_expired() writing ValidationFailure). Don't run
  // -- and crucially don't let the checks below overwrite -- that result: an
  // expired uniquifier combined with an invalid attached contract must still
  // surface as the deterministic ValidationFailure, not be downgraded to the
  // contract-verification error (which would route through
  // commit_body_discarded and report a late replay-cache NonDeterministicError
  // instead).
  if (!cumulative_invoke_result_.is_root_success()) {
    return;
  }

  // Validate the proposal data structure before execution begins.
  // This checks well-formedness (e.g. bit count matches byte count,
  // no entropy grinding via padding bits).
  if (std::holds_alternative<ValidationModeData>(execution_mode_data_)) {
    auto &validation_data = std::get<ValidationModeData>(execution_mode_data_);
    if (!validation_data.validate()) {
      cumulative_invoke_result_.result_ =
          InvokeError{InvokeErrorCode::ValidationFailure};
      return;
    }
  }

  // TODO(geoff): Adjust if some assumption on the rest of the system changes
  //
  // At this point, we do not know the provenance of signed_tx_.  As such,
  // an attached wasm may or may not have been validated by this point.
  //
  // We should validate wasms as they are sent to us from clients (before
  // broadcasting to other nodes) and we may wish to validate them as they are
  // broadcast to us from other nodes. But until we have a concrete design of
  // the coordinator and know what is is and is not doing, we conservatively
  // validate wasms here.
  //
  // If we make the assumption that all wasms are validated by the time they get
  // to the executor, this check could be removed.

  for (auto const &[_, contract] : newly_created_contracts_) {
    auto res = runtime_context_.verify_contract(*contract);
    // TODO(geoff): gas costs for verifying contracts
    if (res != InvokeErrorCode::None) {
      error("Contract verification failed");
      cumulative_invoke_result_.result_ = InvokeError{res};
      return;
    }
  }
}

void
SystemCallHandler::check_final_validation_data()
{
  if (std::holds_alternative<ValidationModeData>(execution_mode_data_)) {
    auto &validation_data = std::get<ValidationModeData>(execution_mode_data_);
    if (!validation_data.check_all_consumed()) {
      // Allowlist: also excludes overwriting an existing ValidationFailure.
      if (cumulative_invoke_result_.is_root_success() ||
          cumulative_invoke_result_.result_.error().code() ==
              InvokeErrorCode::Deterministic ||
          cumulative_invoke_result_.result_.error().code() ==
              InvokeErrorCode::OutOfGas) {
        cumulative_invoke_result_.result_ =
            InvokeError{InvokeErrorCode::ValidationFailure};
      }
    }
  }
}

InvokeResult
SystemCallHandler::root_invoke()
{
  perform_initial_validation();
  // if validation fails, cumulative_invoke_result_ is set to the appropriate
  // error and we return it immediately
  if (!cumulative_invoke_result_.is_root_success()) {
    check_final_validation_data();
    return cumulative_invoke_result_;
  }

  // root_invocation_ is nullopt only in the test harness
  CHECK(root_invocation_.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  Invocation root_invocation = std::move(root_invocation_.value());
  root_invocation_.reset();

  GasAmount available_gas =
      cumulative_invoke_result_.gas_consumed_ < root_invocation.gas_limit
          ? root_invocation.gas_limit - cumulative_invoke_result_.gas_consumed_
          : 0;

  auto [res, gas_consumed] =
      invoke(root_invocation.invoked_address,
             sdk_method_id(root_invocation.entrypoint), available_gas,
             std::move(root_invocation.calldata),
             std::to_underlying(InvokeFlag::None), {});

  cumulative_invoke_result_.result_ = handle_root_host_fn_status(res);
  cumulative_invoke_result_.gas_consumed_ += gas_consumed;

  finalize_after_root_frame();

  return cumulative_invoke_result_;
}

void
SystemCallHandler::finalize_after_root_frame()
{
  assert(!finalized_);
  assert(invocation_frame_stack_.empty());
  finalized_ = true;

  check_final_validation_data();

  if (cumulative_invoke_result_.is_root_success()) {
    root_invoke_succeeded_ = true;
  }
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

ExecResult
SystemCallHandler::try_commit_transaction()
{
  assert(!transaction_committed_or_aborted_);
  if (!cumulative_invoke_result_.is_root_success()) {
    auto error_code = cumulative_invoke_result_.result_.error().code();
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
      return commit_body_discarded(to_exec_result(error_code));
    }

    // System / unrecoverable / validation errors: the transaction is not
    // included and consumes nothing.
    auto res = abort_transaction();
    if (!res.has_value()) {
      return to_exec_result(res.error().code());
    }

    return to_exec_result(error_code);
  }
  assert(last_frame_.has_value());
  assert(root_invoke_succeeded_);
  assert(cumulative_invoke_result_.result_.has_value());

  // If the last frame was set to manually revert, then we commit the
  // transaction as a no-op: body effects are discarded, but RCQ observations
  // and the replay nonce are still committed (see commit_body_discarded).
  if (last_frame_ // NOLINT(bugprone-unchecked-optional-access)
          .value()
          .should_revert()) {
    // Extract the events so that the reverted transaction's events cannot be
    // sent later to the event server.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    last_frame_.value().extract_events();
    return commit_body_discarded(ExecResult::ExecResultTxRootFrameReverted);
  }

  // Root commit: push the accumulated delta batch in one HogDB subtransaction.
  // The child TryInvoke depth cap reserves one slot for this push.
  //
  // Validation mode pushes no per-frame subtransactions, so the root frame's
  // try_invoke_frame_count_ must still be 0 here. This is load-bearing: if a
  // future refactor bumps it in validation, mark_frame_as_failed on a later
  // failure path would pop_subtransactions a savepoint that was never pushed.
  assert(!std::holds_alternative<ValidationModeData>(execution_mode_data_) ||
         last_frame_ // NOLINT(bugprone-unchecked-optional-access)
                 .value()
                 .get_try_invoke_frame_count() == 0);

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

  auto r = last_frame_ // NOLINT(bugprone-unchecked-optional-access)
               .value()
               .try_acquire_frame_locks(db_proxy_, observations);
  if (!r.has_value()) {
    two_phase_commit_failed_ = true;
    auto abort_res = abort_transaction();
    if (!abort_res.has_value()) {
      return to_exec_result(abort_res.error().code());
    }
    return to_exec_result(r.error().code());
  }

  bool result = r.value();
  // check if any of the 2pc database operations failed
  if (!result) {
    two_phase_commit_failed_ = true;

    auto abort_status = abort_transaction();
    if (!abort_status.has_value()) {
      return to_exec_result(abort_status.error().code());
    }

    // abort_transaction() sets transaction_committed_or_aborted_ to true
    return ExecResult::ExecResultTxNonDeterministicError;
  }

  auto new_contract_hashes =
      last_frame_ // NOLINT(bugprone-unchecked-optional-access)
          .value()
          .get_committed_contracts();
  auto res = db_proxy_.commit_transaction(
      BorrowedContractBatch(new_contract_hashes, newly_created_contracts_));
  if (!res.has_value()) {
    two_phase_commit_failed_ = true;
    auto abort_res = abort_transaction();
    if (!abort_res.has_value()) {
      return to_exec_result(abort_res.error().code());
    }
    return to_exec_result(res.error().code());
  }

  // This is past the point of successful commitment;
  // now we can safely emit the events to the event server

  HogexecMetrics::mark_tx_success(cumulative_invoke_result_,
                                  get_block_execution_mode());
  transaction_committed_or_aborted_ = true;
  return ExecResult::ExecResultOK;
}

SystemStatus<void>
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
  HogexecMetrics::mark_tx_failed(cumulative_invoke_result_,
                                 get_block_execution_mode());
  return db_proxy_.abort_transaction();
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
  // The replay cache is disabled for the initial token claim precompile (it
  // provides its own replay protection and a replay cache there is a DoS
  // vector); consume no uniquifier for transactions rooted at it. HogDB rejects
  // a replay-cache delta keyed here (kInvalidExecutorBehavior), so this skip is
  // load-bearing, not an optimization. See kInitialTokenClaimPrecompileAddress.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  if (root_invoked_address_.value() == kInitialTokenClaimPrecompileAddress) {
    return;
  }
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  db_proxy_.add_delta(AddressAndKey{.address = root_invoked_address_.value(),
                                    .key = StorageKey(
                                        StorageKeyType::ReplayCache)},
                      ReplayCacheItem{tx_uniquifier_.value()}.get_delta(),
                      std::to_underlying(StorageDeltaFlags::None));
}

ExecResult
SystemCallHandler::commit_body_discarded(ExecResult success_result)
{
  // In validation mode, RCQ observation deltas must reach HogDB so block-close
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

  auto push_res = db_proxy_.try_push_current_subtransaction();
  if (!push_res.has_value()) {
    // A system error (e.g. a stale block) leaves the HogDB InvokeTX open; abort
    // it before returning, mirroring the root-success lock-push failure path.
    two_phase_commit_failed_ = true;
    auto abort_status = abort_transaction();
    if (!abort_status.has_value()) {
      return to_exec_result(abort_status.error().code());
    }
    return to_exec_result(push_res.error().code());
  }
  if (!push_res.value()) {
    two_phase_commit_failed_ = true;
    auto abort_status = abort_transaction();
    if (!abort_status.has_value()) {
      return to_exec_result(abort_status.error().code());
    }
    return ExecResult::ExecResultTxNonDeterministicError;
  }

  auto res = db_proxy_.commit_transaction();
  HogexecMetrics::mark_tx_failed(cumulative_invoke_result_,
                                 get_block_execution_mode());
  if (!res.has_value()) {
    return to_exec_result(res.error().code());
  }

  transaction_committed_or_aborted_ = true;
  return success_result;
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

std::pair<SystemStatus<SyscallStatus<InvocationFrameStatusCode>>, GasAmount>
SystemCallHandler::invoke(ContractAddress const &address,
                          Runtime::MethodID method, GasAmount gas_limit,
                          Calldata calldata, InvokeFlagSet flags,
                          std::span<uint32_t const> owned_auth_token_ids)
{
  CHECK(validate_invoke_flags(flags));
  if (!invocation_frame_stack_.empty() && is_read_only()) {
    flags |= std::to_underlying(InvokeFlag::ReadOnly);
  }

  // The SyscallError returns below all happen before a child frame is created,
  // so they are complete no-ops: nothing is mutated, and the caller leaves its
  // output state untouched.
  if (!invocation_frame_stack_.empty() &&
      !invocation_frame_stack_.check_auth_tokens_owned(owned_auth_token_ids)) {
    return {SyscallError{
                SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput},
            0};
  }

  // reentrance guard
  // check if the address is already on the stack
  if (!hack_DONTUSE_TEST_BYPASS_allow_reentrance_ &&
      invocation_frame_stack_.has_active_frame_for(address)) {
    return {SyscallError{SyscallErrorCode::kSyscallErrorCodeReentrance}, 0};
  }

  auto runtime = resolve_runtime(address);
  if (!runtime) {
    trace("SystemError: resolving runtime for address: {}, error {}",
          x23::show(address), runtime.error());
    return std::pair{runtime.error(), 0};
  }
  if (!runtime.value()) {
    trace("Failed to instantiate runtime for address: {}", x23::show(address));
    return {SyscallError{
                SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput},
            0};
  }

  Invocation invocation = {.invoked_address = address,
                           .entrypoint = entrypoint_from_method_id(method),
                           .gas_limit = gas_limit,
                           .calldata = std::move(calldata)};

  auto stashed_parent_trace =
      start_frame(invocation, flags, owned_auth_token_ids);

  InvokeResult cumulative_result = runtime.value()->invoke(method, gas_limit);

  auto finish_status =
      finish_frame(cumulative_result.result_, std::move(stashed_parent_trace));
  if (!finish_status.has_value()) {
    // TODO(geoff): This status will result ending execution of the whole block.
    // We can add gas consumption here, but that would only affect the state of
    // the local scheduler, not the canonical chain.
    return {finish_status.error(), 0};
  }
  auto [frame_result, manually_reverted] = finish_status.value();
  cumulative_result += frame_result;

  // A child frame ran. Map its result to a frame status (the success value) or
  // propagate a SystemError. There is no SyscallError here: a frame that ran
  // never produces the "rejected before running" channel.
  auto [frame_status, gas_consumed] = cumulative_result.to_frame_status();
  if (!frame_status.has_value()) {
    return {frame_status.error(), gas_consumed};
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
  return {status_code, gas_consumed};
}

std::unique_ptr<InvocationFrameTrace>
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
  invocation_frame_stack_.start_frame(invocation.invoked_address,
                                      std::move(invocation.calldata), flags,
                                      owned_auth_token_ids);
  return stashed_parent_trace;
}

SystemStatus<std::pair<InvokeResult, bool>>
SystemCallHandler::finish_frame(
    InvokeStatus<void> result,
    std::unique_ptr<InvocationFrameTrace> stashed_parent_trace) noexcept
{
  assert(!invocation_frame_stack_.empty());
  assert(!last_frame_finished_);
  if (!result.has_value()) {
    invocation_frame_stack_.set_active_frame_return(
        InvocationFrameOutput(InvocationFrameStatusCode::Fault), {});
  }

  if (invocation_frame_trace_) {
    invocation_frame_stack_.log_trace_finish(*invocation_frame_trace_);
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
        panic("Failed to attach child trace to parent: {}", e.what());
      }
    }
  }

  [[maybe_unused]] size_t stack_size = invocation_frame_stack_.size();
  auto res = invocation_frame_stack_.finish_frame(result, execution_mode_data_);
  size_t stack_size_after = invocation_frame_stack_.size();
  assert(stack_size_after == stack_size - 1);
  if (stack_size_after == 0) {
    last_frame_finished_ = true;
  }
  return res;
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
    invocation_frame_trace_->system_calls.push_back(SyscallTrace{
        .gas_remaining_at_start = current_gas_remaining,
        // This field is filled in when the syscall finishes.
        .gas_remaining_at_end = std::numeric_limits<GasAmount>::max(),
        // This field is filled in by the individual syscall
        .body = SyscallTraceBody(syscall_number),
        // This field is filled in when the syscall finishes.
        .status = HostFunctionStatusXdr(HostFnErrorCode::Unrecoverable),
    });
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

SystemStatus<const Contract *>
SystemCallHandler::resolve_contract(SHA256Hash const &hash) const
{
  // Check cheap in-memory map first before hitting the snapshot cache
  // (which involves a shared_mutex and potentially a database round-trip).
  auto it = newly_created_contracts_.find(hash);
  if (it != newly_created_contracts_.end()) {
    assert(it->second != nullptr);
    return it->second.get();
  }

  auto snapshot_res = snapshot_cache_.try_get_contract_by_hash(hash);
  if (!snapshot_res.has_value()) {
    return snapshot_res.error();
  }
  auto &snapshot_contract = *snapshot_res;
  if (snapshot_contract != nullptr) {
    return snapshot_contract;
  }
  return nullptr;
}

SystemStatus<std::unique_ptr<Runtime>>
SystemCallHandler::resolve_runtime(ContractAddress const &addr)
{
  // check if the address has a deployed contract hash
  auto deployed_hash_expect =
      snapshot_cache_.try_get_deployed_contract_hash(addr);
  if (!deployed_hash_expect) {
    return deployed_hash_expect.error();
  }
  auto deployed_hash = *deployed_hash_expect;

  if (!deployed_hash) {
    // There was no deployment in the previous snapshot.
    // Now check for the transaction's local context.
    deployed_hash =
        invocation_frame_stack_.lookup_locally_deployed_contract(addr);
  }

  if (!deployed_hash) {
    // A deployment was not found in the local context
    // nor in the snapshot.
    return nullptr;
  }

  // actually make the new runtime
  // TODO(geoff): do verification when contracts are created, not each time.
  return runtime_context_.make_new_runtime(*deployed_hash, this);
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
  assert(cumulative_invoke_result_.has_readable_proposal_data());
  assert(!std::holds_alternative<ViewingModeData>(execution_mode_data_));
  if (std::holds_alternative<ProposalModeData>(execution_mode_data_)) {
    return std::get<ProposalModeData>(execution_mode_data_).proposal_data();
  }
  return std::get<ValidationModeData>(execution_mode_data_).proposal_data();
}

x23::Async<SystemStatus<void>>
SystemCallHandler::emit_events(ExecResult exec_result)
{
  assert(exec_result != ExecResult::ExecResultOK || root_invoke_succeeded_);
  auto events = (root_invoke_succeeded_ && last_frame_.has_value())
                    ? last_frame_.value().extract_events()
                    : x23::Vec<TxEmittedEvent>{};
  auto root_output =
      last_frame_.has_value()
          ? last_frame_.value().get_frame_output()
          : InvocationFrameOutput(InvocationFrameStatusCode::Fault);
  co_return co_await event_server_proxy_.emit_events(TxEventLog{
      .proposed_tx_hash = get_source_tx_hash(),
      .exec_result = exec_result,
      .root_output = std::move(root_output),
      .events = std::move(events),
  });
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
  case HogexecPrecompiledNativeContracts::kNativeContractInitialTokenClaim:
    return true;
  }
  panic("Unknown native contract deployed: {}", native_contract_id);
}
} // namespace

SystemStatus<SyscallStatus<ContractAddress>>
SystemCallHandler::deploy_contract(SHA256Hash const &nonce,
                                   SHA256Hash const &contract_hash)
{
  assert(!invocation_frame_stack_.empty());
  CHECK(!is_read_only());

  // check that contract_hash actually exists
  // This causes a load of the contract to the cache,
  // but this is in most cases no-cost, as a deployed contract will _probably_
  // be immediately invoked with e.g. a constructor
  auto contract_res = resolve_contract(contract_hash);
  if (!contract_res.has_value()) {
    return contract_res.error();
  }
  if (contract_res.value() == nullptr) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  assert(contract_res.value() != nullptr);
  auto const &contract = *contract_res.value();

  // Native (precompiled) contracts cannot be deployed - they are set up at
  // genesis and should not be redeployed by user transactions
  if (contract.contract_type() == ContractType::kNative &&
      is_banned_redeploy_native_contract(contract.native_contract_id())) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  auto deploy_addr =
      static_get_deploy_address(get_current_address(), nonce, contract_hash);

  auto deploy_check_status =
      snapshot_cache_.try_get_deployed_contract_hash(deploy_addr);
  if (!deploy_check_status) {
    return deploy_check_status.error();
  }
  if (*deploy_check_status) {
    return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeContractAlreadyDeployed};
  }

  if (invocation_frame_stack_.lookup_locally_deployed_contract(deploy_addr)
          .has_value()) {
    // contract has already been deployed in this transaction
    return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeContractAlreadyDeployed};
  }

  auto contract_size = [&] -> uint32_t {
    switch (contract.contract_type()) {
    case ContractType::kNative:
    case ContractType::kMockTest:
      return 0LU;
    case ContractType::kWasmi:
      return contract.wasmi_contract().data.size();
    case ContractType::kRv32:
      return contract.rv32_contract().data.size();
    }
    panic("Unrecoverable; Invalid contract type: {}", contract.contract_type());
  }();

  // deploy new contract
  invocation_frame_stack_.deploy_contract(
      deploy_addr,
      DeployedContractHash{.hash = contract_hash,
                           .contract_size = contract_size,
                           .contract_type = contract.contract_type()});

  return deploy_addr;
}

SystemStatus<SyscallStatus<const Bytestring *>>
SystemCallHandler::get_bytestring(StorageKeyContractData const &storage_key)
{
  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);

  if (auto const *bytestring_item = item_view->get<BytestringItem>()) {
    return &bytestring_item->bytestring_;
  }

  if (item_view->get<DeletedItem>()) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist};
  }

  if (auto const *unmodified_item = item_view->get<UnmodifiedItem>()) {
    if (!unmodified_item->is_loaded()) {
      auto addrkey = invocation_frame_stack_.get_addrkey(storage_key);
      auto load_status = unmodified_item->load(snapshot_cache_, addrkey);
      if (!load_status.has_value()) {
        return load_status.error();
      }
    }
    auto const &base_item = unmodified_item->base_item();
    auto type = base_item.type();
    if (!type) {
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist};
    }
    if (*type != ItemViewType::kBytestring) {
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    }
    return &base_item.bytestring();
  }

  return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::set_bytestring(StorageKeyContractData const &storage_key,
                                  std::span<const std::byte> input_bytestring,
                                  StorageDeltaFlagSet input_flags)
{
  CHECK(!is_read_only());

  if (!SystemCallHandler::validate_flags<StorageDeltaFlags::ExclusiveLock>(
          input_flags)) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);
  assert(input_bytestring.size() <= kBytestringMaxLen);

  Bytestring new_string;
  new_string.resize_uninitialized(input_bytestring.size());
  if (!input_bytestring.empty()) {
    assert(new_string.data() != nullptr);
    assert(input_bytestring.data() != nullptr);
    std::memcpy(new_string.data(),
                reinterpret_cast<const uint8_t *>(input_bytestring.data()),
                input_bytestring.size());
  }

  return item_view->set(BytestringItem(std::move(new_string)), input_flags);
}

SystemStatus<SyscallStatus<std::pair<std::optional<int128_t>, int128_t>>>
SystemCallHandler::get_nnint(StorageKeyContractData const &storage_key,
                             bool load_from_snapshot)
{
  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);

  if (auto const *nnint_item = item_view->get<NNIntItem>()) {
    if (nnint_item->base_value_.has_value()) {
      return {std::make_pair(from_xdr(*nnint_item->base_value_),
                             from_xdr(nnint_item->add_value_))};
    }
    // If we're not loading from the snapshot, we can return nullopt for the
    // base value.
    if (!load_from_snapshot) {
      return {std::make_pair(std::nullopt, from_xdr(nnint_item->add_value_))};
    }

    auto addrkey = invocation_frame_stack_.get_addrkey(storage_key);
    auto snapshot_value = snapshot_cache_.try_get_snapshot_item(addrkey);
    if (!snapshot_value.has_value()) {
      return snapshot_value.error();
    }

    // In this case, we're returning the parameters of the prior NNInt write.
    // A bit of a weird case, because we're following an NNintSet(nullopt, add)
    // with a Get(force_load_from_snapshot=true).  We augment the prior NNint
    // write with what the set value will be, when the write is applied to the
    // database. Hence we default to 0 if the snapshot value is not an NNInt.
    auto snapshot_type = snapshot_value.value()->type();
    if (!snapshot_type || snapshot_type.value() != ItemViewType::kNNInt) {
      return std::make_pair(0, from_xdr(nnint_item->add_value_));
    }
    return {std::make_pair(snapshot_value.value()->nnint(),
                           from_xdr(nnint_item->add_value_))};
  }

  if (item_view->get<DeletedItem>()) {
    if (!load_from_snapshot) {
      return {std::make_pair(std::nullopt, 0)};
    }
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist};
  }

  if (auto const *unmodified_item = item_view->get<UnmodifiedItem>()) {
    if (!load_from_snapshot) {
      return {std::make_pair(std::nullopt, 0)};
    }

    if (!unmodified_item->is_loaded()) {
      auto addrkey = invocation_frame_stack_.get_addrkey(storage_key);
      auto load_status = unmodified_item->load(snapshot_cache_, addrkey);
      if (!load_status.has_value()) {
        return load_status.error();
      }
    }
    auto const &base_item = unmodified_item->base_item();
    auto type = base_item.type();
    if (!type) {
      assert(load_from_snapshot);
      // We are in load_from_snapshot mode, so we return KeyNotExist instead of
      // std::nullopt, mirroring the behavior above of the DeletedItem case.
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist};
    }
    if (*type != ItemViewType::kNNInt) {
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    }
    return {std::make_pair(base_item.nnint(), 0)};
  }

  // the value written at this key is not an NNInt, so we return an
  // InvalidSyscallInput error. This mirrors the case above, where the snapshot
  // *type != kNNInt.
  if (!load_from_snapshot) {
    return {std::make_pair(std::nullopt, 0)};
  }
  return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::set_nnint(StorageKeyContractData const &storage_key,
                             std::optional<int128_t> set_value,
                             const int128_t &add_value,
                             StorageDeltaFlagSet input_flags)
{
  CHECK(!is_read_only());

  if (!SystemCallHandler::validate_flags<StorageDeltaFlags::ExclusiveLock>(
          input_flags)) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);

  // Writing to an nnint, without explicitly defining the set value,
  // is defined as equivalent to outputting set_add with set equal to
  // whatever the current database snapshot value is.  As such, it is correct to
  // overwrite the set value here with nullopt, in such a case.
  return item_view->set(NNIntItem{.base_value_ =
                                      set_value.has_value()
                                          ? std::optional<int128_xdr>(
                                                to_xdr(*set_value))
                                          : std::nullopt,
                                  .add_value_ = to_xdr(add_value)},
                        input_flags);
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::delete_object(StorageKeyContractData const &storage_key,
                                 StorageDeltaFlagSet flags)
{
  CHECK(!is_read_only());

  if (!SystemCallHandler::validate_flags<StorageDeltaFlags::ExclusiveLock>(
          flags)) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);
  return item_view->set(DeletedItem{}, flags);
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

x23::Vec<QueueEntry>
get_retrocausally_consumed_entries(ExecutionModeData &execution_mode_data,
                                   SnapshotStorageItem const &base_item,
                                   AddressAndKey const &addrkey, uint32_t count)
{
  if (auto *vd = std::get_if<ValidationModeData>(&execution_mode_data)) {
    return vd->get_observed_queue_entries(addrkey, count);
  }
  if (auto *vwd = std::get_if<ViewingModeData>(&execution_mode_data)) {
    return vwd->get_observed_queue_entries(addrkey, count);
  }
  return base_item.get_retrocausally_consumed_entries(count);
}

SystemStatus<SyscallStatus<RetrocausalQueueItem *>>
retrocausal_queue_item_from_view(ItemView *item_view,
                                 AddressAndKey const &addrkey,
                                 StorageSnapshotCache &snapshot_cache,
                                 ExecutionModeData &execution_mode_data)
{
  if (auto *queue_item = item_view->get<RetrocausalQueueItem>()) {
    return queue_item;
  }
  if (auto const *unmodified_item = item_view->get<UnmodifiedItem>()) {
    if (!unmodified_item->is_loaded()) {
      auto load_status = unmodified_item->load(snapshot_cache, addrkey);
      if (!load_status.has_value()) {
        return load_status.error();
      }
    }
    RetrocausalQueueItem new_item(unmodified_item->base_item());

    auto set_status = item_view->set(new_item);
    if (!set_status.has_value()) {
      return set_status.error();
    }
    return item_view->get<RetrocausalQueueItem>();
  }

  if (item_view->get<DeletedItem>()) {

    UnmodifiedItem unmodified_item;
    assert(!unmodified_item.is_loaded());
    auto load_status = unmodified_item.load(snapshot_cache, addrkey);
    if (!load_status.has_value()) {
      return load_status.error();
    }

    auto consumed = get_retrocausally_consumed_entries(
        execution_mode_data, unmodified_item.base_item(), addrkey,
        item_view->get_retrocausally_consumed_count());
    auto new_item = RetrocausalQueueItem::make_fully_consumed_item(
        unmodified_item.base_item(), std::move(consumed));

    auto set_status = item_view->set(new_item);
    if (!set_status.has_value()) {
      return set_status.error();
    }
    return item_view->get<RetrocausalQueueItem>();
  }

  return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
}

SystemStatus<SyscallStatus<QueueEntry>>
load_next_queue_entry(RetrocausalQueueItem &queue_item,
                      ExecutionModeData &execution_mode_data,
                      bool advance_cursor)
{
  auto entry = queue_item.get_next_item(execution_mode_data, advance_cursor);
  if (!entry.has_value()) {
    return entry.error();
  }

  auto &value = entry.value();
  if (value.has_value()) {
    return value.value();
  }

  return SyscallError{
      SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry};
}

SystemStatus<SyscallStatus<QueueEntry>>
retrocausal_queue_peek_from_view(ItemView *item_view,
                                 AddressAndKey const &addrkey,
                                 StorageSnapshotCache &snapshot_cache,
                                 ExecutionModeData &execution_mode_data)
{
  if (auto *queue_item = item_view->get<RetrocausalQueueItem>()) {
    return load_next_queue_entry(*queue_item, execution_mode_data,
                                 /*advance_cursor=*/false);
  }

  if (auto const *unmodified_item = item_view->get<UnmodifiedItem>()) {
    if (!unmodified_item->is_loaded()) {
      auto load_status = unmodified_item->load(snapshot_cache, addrkey);
      if (!load_status.has_value()) {
        return load_status.error();
      }
    }

    RetrocausalQueueItem queue_item(unmodified_item->base_item());
    return load_next_queue_entry(queue_item, execution_mode_data,
                                 /*advance_cursor=*/false);
  }

  if (item_view->get<DeletedItem>()) {
    UnmodifiedItem unmodified_item;
    auto load_status = unmodified_item.load(snapshot_cache, addrkey);
    if (!load_status.has_value()) {
      return load_status.error();
    }

    auto consumed = get_retrocausally_consumed_entries(
        execution_mode_data, unmodified_item.base_item(), addrkey,
        item_view->get_retrocausally_consumed_count());
    auto queue_item = RetrocausalQueueItem::make_fully_consumed_item(
        unmodified_item.base_item(), std::move(consumed));
    return load_next_queue_entry(queue_item, execution_mode_data,
                                 /*advance_cursor=*/false);
  }

  return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
}
} // namespace

SystemStatus<SyscallStatus<void>>
SystemCallHandler::retrocausal_queue_insert(
    StorageKeyContractData const &key, std::span<const std::byte> queue_entry)
{
  if (queue_entry.size() > kQueueEntryMaxLen) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  auto addrkey = invocation_frame_stack_.get_addrkey(key);
  auto *item_view = invocation_frame_stack_.get_item_view(addrkey);
  assert(item_view);

  auto queue_item =
      retrocausal_queue_item_from_view(item_view, addrkey, snapshot_cache_,
                                       execution_mode_data_);
  if (!queue_item.has_value()) {
    return queue_item.error();
  }
  if (!queue_item.value().has_value()) {
    return queue_item.value().error();
  }

  // Now we have either grabbed the preexisting RCQItem, or created a new one.
  // In either case, we can now insert the new entry.
  return queue_item.value().value()->insert_entry(
      queue_entry_from_span(queue_entry));
}

SystemStatus<SyscallStatus<QueueEntry>>
SystemCallHandler::retrocausal_queue_consume(StorageKeyContractData const &key,
                                             bool peek)
{
  auto addrkey = invocation_frame_stack_.get_addrkey(key);
  auto *item_view = invocation_frame_stack_.get_item_view(addrkey);
  assert(item_view);

  if (peek) {
    return retrocausal_queue_peek_from_view(item_view, addrkey, snapshot_cache_,
                                            execution_mode_data_);
  }

  // For unmodified keys, probe with a local item first. Only persist
  // into the view if an entry is found, so that a no-op consume on an
  // absent key does not pin the storage type.
  if (auto const *unmodified_item = item_view->get<UnmodifiedItem>()) {
    if (!unmodified_item->is_loaded()) {
      auto load_status = unmodified_item->load(snapshot_cache_, addrkey);
      if (!load_status.has_value()) {
        return load_status.error();
      }
    }
    RetrocausalQueueItem queue_item(unmodified_item->base_item());
    auto entry = load_next_queue_entry(queue_item, execution_mode_data_,
                                       /*advance_cursor=*/true);
    if (entry.has_value() && entry.value().has_value()) {
      auto set_status = item_view->set(queue_item);
      if (!set_status.has_value()) {
        return set_status.error();
      }
      item_view->sync_retrocausally_consumed(queue_item);
    }
    return entry;
  }

  if (auto *queue_item = item_view->get<RetrocausalQueueItem>()) {
    auto entry = load_next_queue_entry(*queue_item, execution_mode_data_,
                                       /*advance_cursor=*/true);
    item_view->sync_retrocausally_consumed(*queue_item);
    return entry;
  }

  // Same treatment for deleted keys: a no-op consume should leave the
  // key deleted rather than converting it to an empty RCQ.
  if (item_view->get<DeletedItem>()) {
    UnmodifiedItem unmodified_item;
    auto load_status = unmodified_item.load(snapshot_cache_, addrkey);
    if (!load_status.has_value()) {
      return load_status.error();
    }
    auto consumed = get_retrocausally_consumed_entries(
        execution_mode_data_, unmodified_item.base_item(), addrkey,
        item_view->get_retrocausally_consumed_count());
    auto queue_item = RetrocausalQueueItem::make_fully_consumed_item(
        unmodified_item.base_item(), std::move(consumed));
    auto entry = load_next_queue_entry(queue_item, execution_mode_data_,
                                       /*advance_cursor=*/true);
    if (entry.has_value() && entry.value().has_value()) {
      auto set_status = item_view->set(queue_item);
      if (!set_status.has_value()) {
        return set_status.error();
      }
      item_view->sync_retrocausally_consumed(queue_item);
    }
    return entry;
  }

  return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
}

// == Native token syscalls ==

SystemStatus<SyscallStatus<int128_t>>
SystemCallHandler::get_native_token_balance(
    ContractAddress const &contract_address)
{
  AddressAndKey addrkey{.address = contract_address,
                        .key =
                            StorageKey(StorageKeyType::FreeNativeTokenBalance)};

  auto *item_view = invocation_frame_stack_.get_item_view(addrkey);
  assert(item_view);

  if (auto *unmodified_item = item_view->get<UnmodifiedItem>()) {
    if (!unmodified_item->is_loaded()) {
      auto load_status = unmodified_item->load(snapshot_cache_, addrkey);
      if (!load_status.has_value()) {
        return load_status.error();
      }
    }
    auto const &base_item = unmodified_item->base_item();
    assert(base_item.type() == ItemViewType::kNativeTokenBalance);
    assert(base_item.native_token_balance() >= 0);
    return base_item.native_token_balance();
  }

  auto const *balance_item = item_view->get<NativeTokenBalanceItem>();
  assert(balance_item);

  auto base = [&] -> SystemStatus<int128_t> {
    if (balance_item->base_value_.has_value()) {
      return from_xdr(*balance_item->base_value_);
    }
    auto snapshot_value = snapshot_cache_.try_get_snapshot_item(addrkey);
    if (!snapshot_value.has_value()) {
      return snapshot_value.error();
    }
    auto snapshot_type = snapshot_value.value()->type();
    if (!snapshot_type.has_value()) {
      return 0;
    }
    assert(snapshot_type.value() == ItemViewType::kNativeTokenBalance);
    return snapshot_value.value()->native_token_balance();
  }();

  if (!base.has_value()) {
    return base.error();
  }
  if (integer_add_overflow(*base, from_xdr(balance_item->add_value_))) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  int128_t balance = *base + from_xdr(balance_item->add_value_);
  if (balance < 0) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  return balance;
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::modify_native_token_balance(
    ContractAddress const &contract_address, int128_t transfer_amount)
{
  assert(!invocation_frame_stack_.empty());
  if (is_read_only()) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  AddressAndKey addrkey{.address = contract_address,
                        .key =
                            StorageKey(StorageKeyType::FreeNativeTokenBalance)};

  auto *item_view = invocation_frame_stack_.get_item_view(addrkey);
  assert(item_view);

  if (auto const *balance_item = item_view->get<NativeTokenBalanceItem>()) {
    auto current_add = from_xdr(balance_item->add_value_);
    if (integer_add_overflow(current_add, transfer_amount)) {
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    }
    auto new_add = current_add + transfer_amount;
    return item_view->set(NativeTokenBalanceItem(
        NNIntItem{.base_value_ = balance_item->base_value_,
                  .add_value_ = to_xdr(new_add)}));
  }

  assert(item_view->get<UnmodifiedItem>());
  return item_view->set(
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
} // namespace

SystemStatus<SyscallStatus<void>>
SystemCallHandler::change_public_key_registration(uint32_t key_index,
                                                  PublicKeyConfigOpt new_key)
{
  if (key_index >= kMaxSigners) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  assert(!invocation_frame_stack_.empty());
  auto addr = get_current_address();
  AddressAndKey addrkey = auth_data_addrkey(addr);

  auto *item_view = invocation_frame_stack_.get_item_view(addrkey);
  assert(item_view);

  if (auto *address_auth_data_item = item_view->get<AddressAuthDataItem>()) {
    address_auth_data_item->change_public_key(key_index, std::move(new_key));
    return {};
  }

  if (item_view->get<DeletedItem>()) [[unlikely]] {
    error("Unrecoverable: unexpected manual delete of authentication data");
    return SystemError{SystemErrorCode::Unrecoverable};
  }

  assert(item_view->get<UnmodifiedItem>());
  AddressAuthDataItem new_item;
  new_item.change_public_key(key_index, std::move(new_key));
  return item_view->set(new_item);
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::set_address_required_weight(Weight required_weight)
{
  auto addr = get_current_address();
  auto addrkey = auth_data_addrkey(addr);
  auto *item_view = invocation_frame_stack_.get_item_view(addrkey);
  assert(item_view);

  if (auto *address_auth_data_item = item_view->get<AddressAuthDataItem>()) {
    address_auth_data_item->set_required_weight(required_weight);
    return {};
  }

  if (item_view->get<DeletedItem>()) [[unlikely]] {
    error("Unrecoverable: unexpected manual delete of authentication data");
    return SystemError{SystemErrorCode::Unrecoverable};
  }
  assert(item_view->get<UnmodifiedItem>());
  AddressAuthDataItem new_item;
  new_item.set_required_weight(required_weight);
  return item_view->set(new_item);
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

SystemStatus<BlockHeight>
SystemCallHandler::get_block_height() const
{
  auto tip = snapshot_cache_.try_get_db_tip();
  if (!tip.has_value()) {
    return tip.error();
  }
  return tip.value()->height;
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
