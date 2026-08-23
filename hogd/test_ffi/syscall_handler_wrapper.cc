#include "hogd/test_ffi/syscall_handler_wrapper.h"

#include "auth_claim.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/execution_mode.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/ffi_data_buffer.h"
#include "hogd/test_ffi/mock_runtime.h"
#include "hogd/test_ffi/test_bindings.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include <algorithm>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <utility>

namespace hogd {

SyscallHandlerContext::SyscallHandlerContext(
    std::shared_ptr<MockRuntimeContext> context, SignedTransaction const &stx,
    DBContext &db)
  : identifier_(db.allocate_identifier()), db_(db), open_handler_guard_(db),
    runtime_context_owner_(std::move(context)),
    handler_(stx.tx, db.get_snapshot(), db.get_new_db_proxy(identifier_),
             db.get_new_event_server_proxy(identifier_), ProposalModeCtor{},
             *runtime_context_owner_),
    is_mocked_handler_(false)
{}

SyscallHandlerContext::SyscallHandlerContext(
    std::shared_ptr<MockRuntimeContext> context, DBContext &db,
    AuthClaim const &auth_claim, SHA256Hash const &source_tx_hash)
  : identifier_(db.allocate_identifier()), db_(db), open_handler_guard_(db),
    runtime_context_owner_(std::move(context)),
    handler_(db.get_snapshot(), db.get_new_db_proxy(identifier_),
             db.get_new_event_server_proxy(identifier_), ProposalModeCtor{},
             *runtime_context_owner_, auth_claim, source_tx_hash),
    is_mocked_handler_(true)
{}

ForeignTracedInvokeResult
SyscallHandlerContext::root_invoke(bool enable_tracing)
{
  if (is_mocked_handler_) {
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  }

  // Reject root_invoke() after the transaction was committed or aborted.
  // SystemCallHandler::root_invoke() does not consult transaction_committed_or_
  // aborted_, so a post-abort call would run a fresh root frame whose pending
  // events the one-time abort cleanup never drains, making a later close trip
  // the debug destructor's "destroyed with events to emit" panic (issue #2679).
  if (is_committed_or_aborted()) {
    error("root_invoke called on an already committed/aborted transaction");
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  }

  // root_invoke() is one-shot: a successful first call consumes
  // root_invocation_ and finishes the root frame (last_frame_finished()). A
  // first call that threw during pre-frame setup also consumes root_invocation_
  // but leaves last_frame_finished() false, so root_invoke_attempted_ catches
  // that retry too. Either way a second call would hit the internal
  // CHECK(root_invocation_.has_value()) and abort the process, so reject it
  // with a structured FFI error instead (issues #2435, #2643).
  if (handler_.last_frame_finished() || root_invoke_attempted_) {
    error("root_invoke called more than once on the same handler");
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  }

  if (!push_context()) {
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  }
  // Claim ownership of the TLS context for the duration of the root call so a
  // mock contract callback can't pop it via the exported pop_context() helper
  // and trip the deferred pop's CHECK (issue #2510).
  set_root_invoke_owns_context(true);
  x23::Defer _([this]() {
    set_root_invoke_owns_context(false);
    pop_context();
  });

  // Enable tracing only after push_context() has accepted this root call.
  // enable_tracing() asserts the invocation-frame stack is empty; a reentrant
  // root_invoke() from a mock contract callback runs while the outer root frame
  // is still active, so applying tracing before push_context() would trip that
  // assert ahead of the structured reentry rejection that push_context() (and
  // the root_invoke_attempted_ guard above) provide (issues #2601, #2705). The
  // Defer above is already installed, so a throwing enable_tracing() still pops
  // the context.
  if (enable_tracing) {
    handler_.enable_tracing();
  }

  // Mark the attempt before entering the core call: root_invocation_ is
  // consumed early in SystemCallHandler::root_invoke(), so even a pre-frame
  // throw must block a later retry (issue #2643).
  root_invoke_attempted_ = true;
  auto res = handler_.root_invoke()();

  if (res.result_.has_value() ||
      res.result_.error().code() == InvokeErrorCode::Deterministic ||
      res.result_.error().code() == InvokeErrorCode::OutOfGas ||
      res.result_.error().code() == InvokeErrorCode::ValidationFailure) {

    // handler_.root_invoke() above finalized the root frame, so the result is
    // committable: the caller can hand it to try_commit_transaction(). A
    // proposal-mode ValidationFailure means an expired replay uniquifier
    // rejected before any frame ran (output is a Fault); try_commit_transaction
    // then returns ExecResultTxValidationFailure.
    //
    // If we then fail to hand the finalized result back to the caller, it was
    // never received; poison the handler so try_commit_transaction() refuses to
    // commit a hidden result (issue #2508), mirroring
    // finish_mock_invocation_frame(). The poison runs through this scope-exit
    // guard rather than only at the structured-error return sites so it also
    // fires when a by-value copy throws -- get_most_recent_frame_output() (the
    // root output) or copy_last_frame_events() (the root event vector, #2699)
    // -- not just on serialize_to_ffi_data_buffer() errors. It is gated on
    // last_frame_finished() because a proposal-mode ValidationFailure rejects
    // before any frame runs, so there is no finalized result to hide then.
    bool result_handed_off = false;
    x23::Defer poison_handler_on_handoff_failure([&]() {
      if (!result_handed_off && handler_.last_frame_finished()) {
        mark_finish_result_handoff_failed();
      }
    });

    // The poison guard above owns marking the handler; this just builds the
    // structured FFI error returned to the caller on serialization failure.
    auto serialization_handoff_failure = [](TestFFIErrorCode code) {
      return kMakeFFIError(code);
    };

    auto root_invocation_output = handler_.get_most_recent_frame_output();
    auto serialized_output =
        serialize_to_ffi_data_buffer(root_invocation_output);
    if (!serialized_output.has_value()) {
      return serialization_handoff_failure(serialized_output.error());
    }

    // serialized_output now owns a heap buffer. A later serialization failure
    // returns an FFI error with null buffers, so the caller cannot reclaim it;
    // free the already-allocated buffers on those error paths.
    FFIDataBuffer serialized_trace = kNullBuffer;
    FFIDataBuffer serialized_events = kNullBuffer;
    bool buffers_handed_off = false;
    x23::Defer free_buffers_on_error([&]() {
      if (!buffers_handed_off) {
        hogd::free_ffi_data_buffer(*serialized_output);
        hogd::free_ffi_data_buffer(serialized_trace);
        hogd::free_ffi_data_buffer(serialized_events);
      }
    });

    auto const *trace = handler_.get_trace();
    if (trace) {
      auto s = serialize_to_ffi_data_buffer(*trace);
      if (!s.has_value()) {
        return serialization_handoff_failure(s.error());
      }
      serialized_trace = s.value();
    }

    // Only return events when the invocation succeeded and the root frame
    // returned normally (not aborted/faulted).
    if (res.result_.has_value() && root_invocation_output.status_code() ==
                                       InvocationFrameStatusCode::Return) {
      auto frame_events = handler_.copy_last_frame_events();
      if (!frame_events.empty()) {
        auto s = serialize_to_ffi_data_buffer(frame_events);
        if (!s.has_value()) {
          return serialization_handoff_failure(s.error());
        }
        serialized_events = s.value();
      }
    }

    buffers_handed_off = true;
    result_handed_off = true;
    return ForeignTracedInvokeResult{
        .serialized_output = serialized_output.value(),
        .gas_consumed = res.gas_consumed_,
        .serialized_trace = serialized_trace,
        .serialized_events = serialized_events,
        .ffi_error_code = kTestFFIErrorCodeNone,
    };
  }

  CHECK(!res.result_.has_value());

  switch (res.result_.error().code()) {
  case InvokeErrorCode::None:
  case InvokeErrorCode::Deterministic:
  case InvokeErrorCode::OutOfGas:
  // ValidationFailure (an expired uniquifier in proposal mode) is handled as a
  // committable result above, so it does not reach here.
  case InvokeErrorCode::ValidationFailure:
    panic("unreachable per above checks");
  case InvokeErrorCode::Unrecoverable:
    error("Unrecoverable: root_invoke got Unrecoverable");
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  case InvokeErrorCode::Abandon:
    // Abandon should be unreachable in the test harness.
    // Can only occur if someone holds a tx open across a call to advancing the
    // db block
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  }
  error("Unrecoverable: unrecognized invoke error code: {}",
        res.result_.error().code());
  return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
}

TestFFIErrorCode
SyscallHandlerContext::start_mock_invocation_frame(Invocation const &invocation,
                                                   InvokeFlagSet invoke_flags)
{
  if (!is_mocked_handler_) {
    error("unrecoverable: start_mock_invocation_frame called on non-mocked "
          "handler");
    return kTestFFIErrorCodeUnrecoverableError;
  }

  // Reject opening a mock frame after the transaction was committed or aborted.
  // abort_transaction() drains only the events present at abort time, so a
  // frame opened afterward (and later finished) would publish a fresh
  // successful root result whose events the one-time cleanup never drains,
  // making a later close trip the debug destructor's "destroyed with events to
  // emit" panic (#2680).
  if (is_committed_or_aborted()) {
    error("unrecoverable: start_mock_invocation_frame called on an already "
          "committed/aborted transaction");
    return kTestFFIErrorCodeUnrecoverableError;
  }

  if (handler_.last_frame_finished()) {
    error("unrecoverable: start_mock_invocation_frame called after last frame "
          "finished");
    return kTestFFIErrorCodeUnrecoverableError;
  }

  GasAmount current_gas = std::numeric_limits<GasAmount>::max();
  if (auto *runtime = get_mock_runtime()) {
    auto *gas_runtime = dynamic_cast<MockGasRuntime *>(runtime);
    CHECK(gas_runtime != nullptr);
    current_gas = gas_runtime->gas_remaining();
  }

  // Every exit owes the parent's synthetic kInvoke a finish_syscall_trace();
  // throwing ones used to skip it, leaving the parent to serialize a child
  // Invoke that never opened a frame (#2747). Self-disarming because finishing
  // twice trips the sentinel CHECK and aborts (#2948).
  bool syscall_trace_pending = false;
  auto finish_pending_trace = [&](HostFnStatus<SyscallErrorCode> status) {
    if (syscall_trace_pending) {
      syscall_trace_pending = false;
      handler_.finish_syscall_trace(status, current_gas);
    }
  };
  // try/catch: this runs during unwinding, where an escaping throw terminates.
  x23::Defer complete_trace_on_unwind([&]() {
    try {
      finish_pending_trace(HostFnError{HostFnErrorCode::Unrecoverable});
    } catch (std::exception const &e) {
      report_dropped_log(e.what());
    } catch (...) {
      report_dropped_log("non-std exception completing mock kInvoke trace");
    }
  });

  if (handler_.is_running()) {
    handler_.start_syscall_trace(SyscallNo::kInvoke, current_gas);
    syscall_trace_pending = true;
    if (auto *invoke_trace =
            handler_.get_syscall_trace_data<SyscallNo::kInvoke>()) {
      invoke_trace->inputs = SyscallInvokeInputs{
          .address = invocation.invoked_address,
          .calldata = invocation.calldata,
          .inputs =
              CopyableInvokeInputs{
                  .entrypoint_id = sdk_method_id(invocation.entrypoint),
                  .gas_limit = invocation.gas_limit,
                  .flags = invoke_flags,
              },
      };
    }
  }

  // Clamp the child's budget to what the parent still has, mirroring
  // production kInvoke: syscall_Invoke_inner()() hands
  // SystemCallHandler::invoke() min(requested gas_limit, remaining_gas), so a
  // child frame can never be admitted with a larger budget than its caller can
  // pay for. Without the clamp a mock child could spend past the parent's
  // budget and the parent's debit in finish_mock_invocation_frame() would then
  // fail (issue #2725). current_gas is GasAmount max on the root frame (no
  // parent mock runtime), where the clamp is a no-op. The syscall trace inputs
  // above deliberately keep the *requested* limit: production records the raw
  // syscall arguments there, before this clamp.
  Invocation frame_invocation = invocation;
  frame_invocation.gas_limit =
      std::min<GasAmount>(invocation.gas_limit, current_gas);

  // Inherit the parent frame's ReadOnly bit, mirroring the admission step in
  // SystemCallHandler::invoke(): a frame opened under a read-only ancestor is
  // itself read-only, so wrap_after_parse()'s read-only gate rejects
  // state-modifying syscalls inside it. Without this, a mock child opened with
  // flags = 0 beneath a ReadOnly parent ran read-write, and syscalls production
  // clamps with kSyscallErrorCodeInvalidSyscallInput (e.g. kLogEvent) succeeded
  // and produced real state changes (issue #3300).
  //
  // is_read_only() reads the active frame, so it may only be consulted while a
  // frame is open; the is_running() guard mirrors production's
  // !invocation_frame_stack_.empty() check and leaves a root mock frame with
  // exactly the flags the caller asked for.
  //
  // As with the gas clamp above, the syscall trace inputs deliberately keep the
  // caller-supplied flags: production records the raw syscall arguments in
  // wrap_after_parse(), before invoke() ORs the inherited bit into the flags it
  // hands to start_frame().
  InvokeFlagSet frame_flags = invoke_flags;
  if (handler_.is_running() && handler_.is_read_only()) {
    frame_flags |= std::to_underlying(InvokeFlag::ReadOnly);
  }

  // The admission checks invoke() runs before creating a child frame: undefined
  // flag bits (#3302) and reentrance (#3301). Auth-token ownership is vacuous
  // until the API below can pass tokens. Only locals have been computed by this
  // point, so a refusal pushes no frame and the caller owes no finish; the
  // caller's synthetic kInvoke is traced with production's syscall error and no
  // gas charged, matching a refused kInvoke. Closed before the error() log,
  // which can throw.
  if (auto admitted = handler_.validate_new_invocation(
          invocation.invoked_address, frame_flags, std::span<uint32_t const>{});
      !admitted.has_value()) {
    finish_pending_trace(admitted.error().code());
    error("invalid argument: mock invocation frame not admitted: {}",
          admitted.error().code());
    return kTestFFIErrorCodeInvalidArgument;
  }

  // TODO(geoff): current API does not support passing auth tokens via the mock
  // frames.
  //
  // start_frame() pushes a frame onto the handler's invocation stack. The
  // trace-stack and mock-runtime bookkeeping that pairs with it must not be
  // able to throw afterward, or the handler would be left with an active frame
  // that has no backing mock runtime and can be neither finished nor closed
  // (issue #2507). Perform the throwing allocations -- the clamped Invocation
  // copy above, the EmptyRuntime, and the two stacks' capacity -- before
  // start_frame() so the publish step below is non-throwing.
  auto mock_runtime = std::make_unique<EmptyRuntime>(frame_invocation);
  invocation_frame_trace_stack_.reserve(invocation_frame_trace_stack_.size() +
                                        1);
  mock_runtime_stack_.reserve(mock_runtime_stack_.size() + 1);

  // Moved, never copied: start_frame() takes the Invocation by value and the
  // copy would allocate its Calldata again after the throwing-allocation window
  // closed above.
  auto stashed_parent_trace =
      handler_.start_frame(std::move(frame_invocation), frame_flags,
                           std::span<uint32_t const>{});
  if (!stashed_parent_trace.has_value()) {
    // Child frame admission failed under memory pressure. start_frame() rolled
    // back, so the handler has no new frame; skip publishing the mock-runtime
    // and trace bookkeeping (which would otherwise leave an active frame with
    // no backing mock runtime, #2507) and surface an unrecoverable FFI error
    // (#3058). The reserved capacity and unreleased mock_runtime above are
    // harmless.
    // The parent is still running; close its kInvoke the way production does,
    // via to_host_fn_error() with no gas consumed.
    finish_pending_trace(stashed_parent_trace.error().to_host_fn_error());
    error("unrecoverable: frame admission allocation failed in "
          "start_mock_invocation_frame");
    return kTestFFIErrorCodeUnrecoverableError;
  }

  // finish_mock_invocation_frame() owns the record now. Must disarm: the
  // child's trace is active from here, and finishing against its empty
  // system_calls trips a CHECK.
  syscall_trace_pending = false;

  // Push exactly one slot per mock frame so this stack stays in lockstep with
  // the handler's invocation-frame depth, and finish_mock_invocation_frame()
  // restores only the trace the finishing frame actually stashed. The slot may
  // be null: the root frame has no parent trace to stash, and a best-effort
  // trace drop (start_syscall_trace()/start_frame() clearing the active trace
  // after an allocation failure, #2644/#2473) leaves start_frame() with nothing
  // to hand back. Pushing conditionally would let a deeper untraced frame's
  // finish pop and restore an ancestor's saved trace one frame too early, so a
  // later finish re-finalizes the same Invoke syscall record and aborts on the
  // gas_remaining_at_end guard in finish_syscall_trace() (#2948). The null
  // placeholder is harmless — finish_frame() already tolerates a null stashed
  // parent trace.
  invocation_frame_trace_stack_.push_back(
      std::move(stashed_parent_trace).value());

  // Capacity was reserved above, so this push_back cannot reallocate or throw.
  push_mock_runtime(mock_runtime.release());
  return kTestFFIErrorCodeNone;
}

ForeignTracedInvokeResult
SyscallHandlerContext::finish_mock_invocation_frame(
    InvokeErrorCode invoke_error_code)
{
  // Error guards on FFI usage
  if (!is_mocked_handler_) {
    error("unrecoverable: finish_mock_invocation_frame called on non-mocked "
          "handler");
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  }

  // Reject finishing a mock frame after the transaction was committed or
  // aborted: finalizing a root frame here would publish a post-terminal root
  // result and its undrained events, mirroring the open-frame guard (#2680).
  if (is_committed_or_aborted()) {
    error("unrecoverable: finish_mock_invocation_frame called on an already "
          "committed/aborted transaction");
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  }

  if (handler_.last_frame_finished()) {
    error("unrecoverable: finish_mock_invocation_frame called after last frame "
          "finished");
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  }

  // A fresh mocked handler (or one whose frames have all been finished without
  // last_frame_finished_ being set) has an empty mock_runtime_stack_. Calling
  // finish here would reach pop_mock_runtime(), whose only empty-stack guard is
  // a debug assert -- aborting Debug builds and reading vector::back() out of
  // bounds in release. Reject the out-of-order finish with a structured error
  // instead (issue #2589). mock_runtime_stack_ is the RAII record of frames the
  // harness still owes a finish (see can_be_closed()).
  if (mock_runtime_stack_.empty()) {
    error("unrecoverable: finish_mock_invocation_frame called with no open "
          "mock invocation frame");
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  }

  // convert InvokeErrorCode to InvokeStatus<void>
  InvokeStatus<void> status_code = (invoke_error_code == InvokeErrorCode::None)
                                       ? InvokeStatus<void>{}
                                       : InvokeError{invoke_error_code};

  // Snapshot events from the frame about to finish BEFORE mutating any wrapper
  // state. copy_active_frame_events() returns the active frame's event vector
  // by value and allocates when the frame has events, so it can throw on
  // allocation failure. Taking the snapshot first keeps this finish
  // failure-atomic: a throw here leaves the mock runtime stack and active
  // invocation frame paired, so the caller can retry, instead of popping the
  // mock runtime out from under a still-running handler frame (which left
  // can_be_closed() lying about an empty stack while is_running() stayed true —
  // issue #2658). finish_frame() below merges these into the parent (success)
  // or discards them (failure).
  auto frame_events = handler_.copy_active_frame_events();

  auto *runtime = pop_mock_runtime();
  CHECK(runtime != nullptr);
  auto *gas_runtime = dynamic_cast<MockGasRuntime *>(runtime);
  CHECK(gas_runtime != nullptr);
  uint64_t consumed_gas = gas_runtime->gas_consumed();
  x23::Defer _([runtime]() { delete runtime; });

  // This frame ran out of gas: either one of its own syscalls outspent its
  // budget, or it was charged for a child frame it could not pay for (the
  // parent debit at the bottom of this function). Either way production gives
  // the caller no say in what happens next -- the OutOfGas host error traps the
  // frame's guest execution, so it faults no matter what its code would rather
  // have returned. The mock pathway has no guest execution to trap, so the
  // caller-supplied status is overridden here. Without this the frame still
  // finished Return and try_commit_transaction() still reported ExecResultOK --
  // committing a transaction production would have faulted (issue #2725).
  //
  // Only a frame-level status is overridden. A SystemError-class code
  // (Unrecoverable / ValidationFailure / Abandon) ends the whole transaction
  // and outranks OutOfGas, so it is never downgraded -- the same exclusion
  // wrap_after_parse() makes via its is_handleable_error guard, and the same
  // one the synthetic kInvoke status below makes.
  if (gas_runtime->is_out_of_gas() &&
      (invoke_error_code == InvokeErrorCode::None ||
       invoke_error_code == InvokeErrorCode::Deterministic ||
       invoke_error_code == InvokeErrorCode::OutOfGas)) {
    status_code = InvokeStatus<void>{InvokeError{InvokeErrorCode::OutOfGas}};
  }

  auto result =
      InvokeResult{.result_ = status_code, .gas_consumed_ = consumed_gas};

  // start_mock_invocation_frame() pushes one slot per mock frame, so this stack
  // is in lockstep with the frames the harness still owes a finish (the same
  // frames mock_runtime_stack_ tracks, already checked non-empty above).
  // Popping exactly one slot here restores the trace this frame stashed — never
  // an ancestor's (#2948). The empty check stays as defensive
  // belt-and-suspenders.
  std::unique_ptr<InvocationFrameTrace> stashed_parent_trace = nullptr;
  if (!invocation_frame_trace_stack_.empty()) {
    stashed_parent_trace = std::move(invocation_frame_trace_stack_.back());
    invocation_frame_trace_stack_.pop_back();
  }

  // finish the frame
  auto res =
      handler_.finish_frame(result.result_, std::move(stashed_parent_trace))();
  if (!res.has_value()) {
    error("unrecoverable: finish_mock_invocation_frame failed: {}",
          res.error());
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  }
  auto [frame_result, manually_reverted] = res.value();
  result += frame_result;
  GasAmount total_gas_consumed = result.gas_consumed_;

  // Debit the parent for the child frame. MockGasRuntime::consume_gas() reports
  // false exactly when the charge meets or exceeds what the parent had left,
  // which is production's `gas_consumed >= gas_remaining` out-of-gas condition
  // in wrap_after_parse()'s gas epilogue. Remember it rather than discarding
  // it: the synthetic kInvoke below used to be recorded as a success on a
  // parent that could not pay for its own child (issue #2725).
  GasAmount gas_remaining = std::numeric_limits<GasAmount>::max();
  bool parent_out_of_gas = false;
  if (auto *runtime = get_mock_runtime()) {
    auto *gas_runtime = dynamic_cast<MockGasRuntime *>(runtime);
    CHECK(gas_runtime != nullptr);
    // A failed debit also latches MockGasRuntime::is_out_of_gas() on the
    // parent, so the parent's own finish faults the way production's trapped
    // guest execution would (see the override at the top of this function).
    // parent_out_of_gas is the local copy used for the synthetic kInvoke status
    // just below.
    parent_out_of_gas = !gas_runtime->consume_gas(total_gas_consumed);
    gas_remaining = gas_runtime->gas_remaining();
  }

  if (handler_.is_running()) {
    auto status = [&]() -> HostFnStatus<SyscallErrorCode> {
      auto frame_status = result.to_frame_status().first;
      // A SystemError (Unrecoverable / Abandon / ValidationFailure) ends the
      // whole transaction and must never be turned into an out-of-gas error;
      // production makes the same exclusion via wrap_after_parse()'s
      // is_handleable_error check, so test the SystemError channel first.
      if (!frame_status.has_value()) {
        return frame_status.error().to_host_fn_error();
      }
      // The child ran, but the parent could not pay for it. Production rewrites
      // a handleable syscall result to OutOfGas here, so the mock synthetic
      // kInvoke has to be classified the same way (issue #2725).
      if (parent_out_of_gas) {
        return HostFnError{HostFnErrorCode::OutOfGas};
      }
      // A child frame that ran (any outcome) makes the Invoke syscall return
      // None; only a SystemError surfaces as a host-function error.
      return SyscallErrorCode::kSyscallErrorCodeNone;
    }();
    handler_.finish_syscall_trace(status, gas_remaining);
  }

  // When the root (last) mock frame finishes, run the same finalization
  // that root_invoke() does so that try_commit_transaction() can proceed.
  // NOTE: mock path starts with gas_consumed_ == 0, so assignment == +=.
  if (handler_.last_frame_finished()) {
    handler_.mock_set_cumulative_invoke_result(result);
    handler_.finalize_after_root_frame();

    // Mirror root_invoke(): a root-level system error (Unrecoverable,
    // ValidationFailure, Abandon) must surface as an FFI error, not a
    // serialized Fault that silently hides the cumulative root system result
    // from the caller until a later try_commit_transaction(). Deterministic
    // and OutOfGas classify as Fault, so they keep flowing through as ordinary
    // serialized Fault outputs.
    if (!result.to_frame_status().first.has_value()) {
      error("Unrecoverable: finish_mock_invocation_frame got root system "
            "error: {}",
            result.result_.error().code());
      return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
    }
  }

  // The frame has been committed: finish_frame() merged a successful child into
  // its parent, and (for the root frame) finalize_after_root_frame() above made
  // the cumulative result committable. From here until the result buffers are
  // handed back to the caller, ANY failure -- a structured serialization error
  // OR a thrown copy/allocation failure -- means the caller never received the
  // result it was told to expect, so the handler must be poisoned and
  // try_commit_transaction() must refuse to commit it.
  //
  // The poison runs through this scope-exit guard rather than only at the
  // structured-error return sites so it also fires when a by-value copy throws,
  // e.g. get_most_recent_frame_output() copying a nonempty ReturnBuffer
  // (#2730). It is unconditional -- not gated on last_frame_finished() -- so it
  // also covers non-root frames, whose child output finish_frame() already
  // merged into the parent before this point; a non-root handoff failure must
  // block the later parent commit too (#2768), mirroring the root case (#2508).
  bool result_handed_off = false;
  x23::Defer poison_handler_on_handoff_failure([&]() {
    if (!result_handed_off) {
      mark_finish_result_handoff_failed();
    }
  });

  auto frame_output = handler_.get_most_recent_frame_output();

  // The poison guard above owns marking the handler; this just builds the
  // structured FFI error returned to the caller on serialization failure.
  auto serialization_handoff_failure = [](TestFFIErrorCode code) {
    return kMakeFFIError(code);
  };

  auto serialized_output = serialize_to_ffi_data_buffer(frame_output);
  if (!serialized_output.has_value()) {
    return serialization_handoff_failure(serialized_output.error());
  }

  // serialized_output now owns a heap buffer. A later serialization failure
  // returns an FFI error with null buffers, so the caller cannot reclaim it;
  // free the already-allocated buffers on those error paths.
  FFIDataBuffer serialized_trace = kNullBuffer;
  FFIDataBuffer serialized_events = kNullBuffer;
  bool buffers_handed_off = false;
  x23::Defer free_buffers_on_error([&]() {
    if (!buffers_handed_off) {
      hogd::free_ffi_data_buffer(serialized_output.value());
      hogd::free_ffi_data_buffer(serialized_trace);
      hogd::free_ffi_data_buffer(serialized_events);
    }
  });

  auto const *trace = handler_.get_trace();
  if (trace) {
    auto s = serialize_to_ffi_data_buffer(*trace);
    if (!s.has_value()) {
      return serialization_handoff_failure(s.error());
    }
    serialized_trace = s.value();
  }

  // Only return events when the frame succeeded — failed or reverted
  // frames discard their events.
  bool frame_succeeded =
      invoke_error_code == InvokeErrorCode::None && !manually_reverted;
  if (frame_succeeded && !frame_events.empty()) {
    auto s = serialize_to_ffi_data_buffer(frame_events);
    if (!s.has_value()) {
      return serialization_handoff_failure(s.error());
    }
    serialized_events = s.value();
  }

  buffers_handed_off = true;
  result_handed_off = true;
  return ForeignTracedInvokeResult{
      .serialized_output = serialized_output.value(),
      .gas_consumed = total_gas_consumed,
      .serialized_trace = serialized_trace,
      .serialized_events = serialized_events,
      .ffi_error_code = kTestFFIErrorCodeNone,
  };
}

} // namespace hogd
