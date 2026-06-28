// Regression test for issue #2705: test_ffi traced reentrant root_invoke
// asserts before the reentry guard.
//
// A mock contract callback that reenters root_invoke() on its own still-running
// handler must be rejected with a structured FFI error -- the same as the
// untraced path -- rather than reaching SystemCallHandler::enable_tracing(),
// whose empty-frame-stack assertion fires while the outer root frame is active.

#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

namespace {

struct ReentrantRootInvokeState {
  void *syscall_handler = nullptr;
  TestFFIErrorCode nested_result = kTestFFIErrorCodeNone;
  uint8_t enable_tracing = 0;
  bool nested_called = false;
};

// A mock root contract callback that tries to reenter root_invoke() on its own
// live handler while the outer root invocation still owns the runtime frame.
ForeignInvokeResult
mock_contract_reentrant_root_invoke(void *ctx, uint64_t method_number)
{
  if (method_number != 0x1234) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }
  auto *state = static_cast<ReentrantRootInvokeState *>(ctx);
  auto result = root_invoke(state->syscall_handler, state->enable_tracing);
  state->nested_result = result.ffi_error_code;
  state->nested_called = true;
  free_ffi_data_buffer(result.serialized_output);
  free_ffi_data_buffer(result.serialized_trace);
  free_ffi_data_buffer(result.serialized_events);
  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

void
run_reentrant_root_invoke_from_mock_callback(ReentrantRootInvokeState &state,
                                             uint8_t nested_enable_tracing)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  state.enable_tracing = nested_enable_tracing;
  ContractAddress deployment_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_reentrant_root_invoke,
                           &state);

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = deployment_addr,
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 },
             .uniquifier = {.block_expiry_height = INT64_MAX, .nonce = 0}},
      .signatures = {},
  };

  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  ASSERT_NE(syscall_handler, nullptr);
  state.syscall_handler = syscall_handler;

  auto result = root_invoke(syscall_handler, /*enable_tracing=*/0);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(result.serialized_output);
  free_ffi_data_buffer(result.serialized_trace);
  free_ffi_data_buffer(result.serialized_events);

  EXPECT_EQ(abort_transaction(syscall_handler), kTestFFIErrorCodeNone);
  EXPECT_EQ(close_syscall_handler(syscall_handler), kTestFFIErrorCodeNone);
}

} // namespace

TEST(TestBindingsTest, ReentrantRootInvokeWithoutTracingRejected)
{
  ReentrantRootInvokeState state;
  run_reentrant_root_invoke_from_mock_callback(state,
                                               /*nested_enable_tracing=*/0);

  EXPECT_TRUE(state.nested_called);
  EXPECT_EQ(state.nested_result, kTestFFIErrorCodeUnrecoverableError);
}

TEST(TestBindingsTest, ReentrantRootInvokeWithTracingRejectedNotAsserted)
{
  ReentrantRootInvokeState state;
  run_reentrant_root_invoke_from_mock_callback(state,
                                               /*nested_enable_tracing=*/1);

  EXPECT_TRUE(state.nested_called);
  EXPECT_EQ(state.nested_result, kTestFFIErrorCodeUnrecoverableError);
}

} // namespace hogd
