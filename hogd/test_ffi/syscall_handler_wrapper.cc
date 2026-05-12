#include "hogd/test_ffi/syscall_handler_wrapper.h"

#include "auth_claim.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/ffi_data_buffer.h"
#include "hogd/test_ffi/mock_runtime.h"
#include "hogd/test_ffi/test_bindings.h"
#include "invoke_flag.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <utility>

namespace hogd {

SyscallHandlerContext::SyscallHandlerContext(MockRuntimeContext *context,
                                             SignedTransaction const &stx,
                                             DBContext &db)
  : identifier_(db.allocate_identifier()), db_(db),
    handler_(stx.tx, db.get_snapshot(), db.get_new_db_proxy(identifier_),
             db.get_new_event_server_proxy(identifier_), ProposalModeCtor{},
             *context),
    is_mocked_handler_(false)

{}

SyscallHandlerContext::SyscallHandlerContext(MockRuntimeContext *context,
                                             DBContext &db,
                                             AuthClaim const &auth_claim,
                                             SHA256Hash const &source_tx_hash)
  : identifier_(db.allocate_identifier()), db_(db),
    handler_(db.get_snapshot(), db.get_new_db_proxy(identifier_),
             db.get_new_event_server_proxy(identifier_), ProposalModeCtor{},
             *context, auth_claim, source_tx_hash),
    is_mocked_handler_(true)
{}

ForeignTracedInvokeResult
SyscallHandlerContext::root_invoke()
{
  if (is_mocked_handler_) {
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  }

  if (!push_context()) {
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  }
  x23::Defer _([this]() { pop_context(); });

  auto res = handler_.root_invoke();

  if (res.result_.has_value() ||
      res.result_.error().code() == InvokeErrorCode::Deterministic ||
      res.result_.error().code() == InvokeErrorCode::OutOfGas) {

    auto root_invocation_output = handler_.get_most_recent_frame_output();
    auto serialized_output =
        serialize_to_ffi_data_buffer(root_invocation_output);
    if (!serialized_output.has_value()) {
      return kMakeFFIError(serialized_output.error());
    }
    FFIDataBuffer serialized_trace = kNullBuffer;
    auto const *trace = handler_.get_trace();
    if (trace) {
      auto s = serialize_to_ffi_data_buffer(*trace);
      if (!s.has_value()) {
        return kMakeFFIError(s.error());
      }
      serialized_trace = s.value();
    }

    // Only return events when the invocation succeeded and the root frame
    // returned normally (not aborted/faulted).
    FFIDataBuffer serialized_events = kNullBuffer;
    if (res.result_.has_value() && root_invocation_output.status_code() ==
                                       InvocationFrameStatusCode::Return) {
      auto frame_events = handler_.copy_last_frame_events();
      if (!frame_events.empty()) {
        auto s = serialize_to_ffi_data_buffer(frame_events);
        if (!s.has_value()) {
          return kMakeFFIError(s.error());
        }
        serialized_events = s.value();
      }
    }

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
    panic("unreachable per above checks");
  case InvokeErrorCode::Unrecoverable:
    error("Unrecoverable: root_invoke got Unrecoverable");
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  case InvokeErrorCode::ValidationFailure:
    // TODO(geoff): this will need to be handled differently if we
    // add support in the SDK for testing transactions in validation mode.
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

  if (handler_.last_frame_finished()) {
    error("unrecoverable: start_mock_invocation_frame called after last frame "
          "finished");
    return kTestFFIErrorCodeUnrecoverableError;
  }

  GasAmount current_gas = std::numeric_limits<GasAmount>::max();
  if (auto const *runtime = get_mock_runtime()) {
    current_gas = runtime->get_available_gas();
  }

  if (handler_.is_running()) {
    handler_.start_syscall_trace(SyscallNo::kInvoke, current_gas);
    if (auto *invoke_trace =
            handler_.get_syscall_trace_data<SyscallNo::kInvoke>()) {
      invoke_trace->inputs = SyscallInvokeInputs{
          .address = invocation.invoked_address,
          .entrypoint_id = sdk_method_id(invocation.entrypoint),
          .gas_limit = invocation.gas_limit,
          .calldata = invocation.calldata,
          .flags = invoke_flags,
      };
    }
  }

  // TODO(geoff): current API does not support passing auth tokens via the mock
  // frames.
  auto stashed_parent_trace = handler_.start_frame(invocation, invoke_flags,
                                                   std::span<uint32_t const>{});
  if (stashed_parent_trace) {
    invocation_frame_trace_stack_.push_back(std::move(stashed_parent_trace));
  }

  push_mock_runtime(new EmptyRuntime(invocation));
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

  if (handler_.last_frame_finished()) {
    error("unrecoverable: finish_mock_invocation_frame called after last frame "
          "finished");
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  }

  // convert InvokeErrorCode to InvokeStatus<void>
  InvokeStatus<void> status_code = (invoke_error_code == InvokeErrorCode::None)
                                       ? InvokeStatus<void>{}
                                       : InvokeError{invoke_error_code};

  // get the runtime and gas consumed by the invocation
  auto *runtime = pop_mock_runtime();
  CHECK(runtime != nullptr);
  auto *empty_runtime = dynamic_cast<EmptyRuntime *>(runtime);
  CHECK(empty_runtime != nullptr);
  uint64_t consumed_gas = empty_runtime->get_consumed_gas();
  x23::Defer _([runtime]() { delete runtime; });

  auto result =
      InvokeResult{.result_ = status_code, .gas_consumed_ = consumed_gas};

  // Snapshot events from the frame about to finish — finish_frame will
  // merge them into the parent (success) or discard them (failure).
  auto frame_events = handler_.copy_active_frame_events();

  std::unique_ptr<InvocationFrameTrace> stashed_parent_trace = nullptr;
  if (!invocation_frame_trace_stack_.empty()) {
    stashed_parent_trace = std::move(invocation_frame_trace_stack_.back());
    invocation_frame_trace_stack_.pop_back();
  }

  // finish the frame
  auto res =
      handler_.finish_frame(result.result_, std::move(stashed_parent_trace));
  if (!res.has_value()) {
    error("unrecoverable: finish_mock_invocation_frame failed: {}",
          res.error());
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  }
  auto [frame_result, manually_reverted] = res.value();
  result += frame_result;

  GasAmount gas_remaining = std::numeric_limits<GasAmount>::max();
  if (auto *runtime = get_mock_runtime()) {
    auto _ = runtime->consume_gas(consumed_gas);
    gas_remaining = runtime->get_available_gas();
  }

  if (handler_.is_running()) {
    auto status = [&]() -> HostFnStatus<SyscallErrorCode> {
      if (result.to_host_fn_result().first.has_value()) {
        return result.to_host_fn_result().first.value();
      }
      return result.to_host_fn_result().first.error().to_host_fn_error();
    }();
    handler_.finish_syscall_trace(status, gas_remaining);
  }

  // When the root (last) mock frame finishes, run the same finalization
  // that root_invoke() does so that try_commit_transaction() can proceed.
  // NOTE: mock path starts with gas_consumed_ == 0, so assignment == +=.
  if (handler_.last_frame_finished()) {
    handler_.mock_set_cumulative_invoke_result(result);
    handler_.finalize_after_root_frame();
  }

  auto frame_output = handler_.get_most_recent_frame_output();

  auto serialized_output = serialize_to_ffi_data_buffer(frame_output);
  if (!serialized_output.has_value()) {
    return kMakeFFIError(serialized_output.error());
  }

  FFIDataBuffer serialized_trace = kNullBuffer;
  auto const *trace = handler_.get_trace();
  if (trace) {
    auto s = serialize_to_ffi_data_buffer(*trace);
    if (!s.has_value()) {
      return kMakeFFIError(s.error());
    }
    serialized_trace = s.value();
  }

  // Only return events when the frame succeeded — failed or reverted
  // frames discard their events.
  bool frame_succeeded =
      invoke_error_code == InvokeErrorCode::None && !manually_reverted;
  FFIDataBuffer serialized_events = kNullBuffer;
  if (frame_succeeded && !frame_events.empty()) {
    auto s = serialize_to_ffi_data_buffer(frame_events);
    if (!s.has_value()) {
      return kMakeFFIError(s.error());
    }
    serialized_events = s.value();
  }

  return ForeignTracedInvokeResult{
      .serialized_output = serialized_output.value(),
      .gas_consumed = consumed_gas + res->first.gas_consumed_,
      .serialized_trace = serialized_trace,
      .serialized_events = serialized_events,
      .ffi_error_code = kTestFFIErrorCodeNone,
  };
}

} // namespace hogd
