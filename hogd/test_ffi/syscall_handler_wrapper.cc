#include "hogd/test_ffi/syscall_handler_wrapper.h"

#include "auth_claim.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/ffi_data_buffer.h"
#include "hogd/test_ffi/mock_runtime.h"
#include "hogd/test_ffi/test_bindings.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

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

SyscallHandlerContext::SyscallHandlerContext(
    MockRuntimeContext *context, DBContext &db,
    AuthClaimData const &auth_claim_data, SHA256Hash const &source_tx_hash)
  : identifier_(db.allocate_identifier()), db_(db),
    handler_(db.get_snapshot(), db.get_new_db_proxy(identifier_),
             db.get_new_event_server_proxy(identifier_), ProposalModeCtor{},
             *context, auth_claim_data, source_tx_hash),
    is_mocked_handler_(true)
{}

ForeignRootInvokeResult
SyscallHandlerContext::root_invoke()
{
  constexpr static auto kNullBuffer = FFIDataBuffer{.data = nullptr, .len = 0};
  constexpr static auto kMakeFFIError =
      [](TestFFIErrorCode code) -> ForeignRootInvokeResult {
    return {
        .serialized_output = kNullBuffer,
        .gas_consumed = 0,
        .serialized_trace = kNullBuffer,
        .ffi_error_code = code,
    };
  };

  if (is_mocked_handler_) {
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  }

  if (!push_context()) {
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  }
  x23::Defer _([this]() { pop_context(); });

  auto res = handler_.root_invoke();

  if (res.result_.has_value() ||
      res.result_.error().code() == InvokeErrorCode::kInvokeDeterministic ||
      res.result_.error().code() == InvokeErrorCode::kInvokeOutOfGas) {

    auto root_invocation_output = handler_.get_root_invocation_output();
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

    return ForeignRootInvokeResult{
        .serialized_output = serialized_output.value(),
        .gas_consumed = res.gas_consumed_,
        .serialized_trace = serialized_trace,
        .ffi_error_code = kTestFFIErrorCodeNone,
    };
  }

  CHECK(!res.result_.has_value());

  switch (res.result_.error().code()) {
  case InvokeErrorCode::kInvokeNone:
  case InvokeErrorCode::kInvokeDeterministic:
  case InvokeErrorCode::kInvokeOutOfGas:
    panic("unreachable per above checks");
  case InvokeErrorCode::kInvokeUnrecoverable:
  case InvokeErrorCode::kInvokeValidationFailure:
    // TODO(geoff): this will need to be handled differently if we
    // add support in the SDK for testing transactions in validation mode.
  case InvokeErrorCode::kInvokeAbortTransaction:
    // kInvokeAbortTransaction should be unreachable in the test harness.
    // Can only occur if someone holds a tx open across a call to advancing the
    // db block
    return kMakeFFIError(kTestFFIErrorCodeUnrecoverableError);
  }
}

} // namespace hogd
