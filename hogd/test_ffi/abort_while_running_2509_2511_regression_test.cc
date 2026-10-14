// Regression tests for issues #2509 and #2511. See test_ffi_test_helpers.h.

#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "invoke_flag.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

// Regression test for #2509: calling abort_transaction() while a mock
// invocation frame is still open used to report success but leave the mock
// runtime stack populated, so the next close_syscall_handler() failed. Abort is
// now rejected while a frame is active; once the frame is finished the handler
// closes cleanly.
TEST(TestBindingsTest, AbortWithOpenMockFrameRejected)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(handler.syscall_handler, nullptr);

  Invocation invocation = {
      .invoked_address = ContractAddress{0x04},
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 10'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);

  // Abort is rejected while the mock frame is still open.
  EXPECT_EQ(abort_transaction(handler.syscall_handler),
            kTestFFIErrorCodeUnrecoverableError);

  // Finishing the frame returns the handler to a closeable state.
  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(finish_res.serialized_output);
  free_ffi_data_buffer(finish_res.serialized_trace);
  free_ffi_data_buffer(finish_res.serialized_events);

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

namespace {

struct ReentrantAbortState {
  void *syscall_handler = nullptr;
  TestFFIErrorCode abort_result = kTestFFIErrorCodeNone;
  bool abort_called = false;
};

// A mock root contract callback that tries to abort its own still-running
// transaction through the live handler pointer.
ForeignInvokeResult
mock_contract_reentrant_abort(void *ctx, uint64_t method_number)
{
  if (method_number != 0x1234) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }
  auto *state = static_cast<ReentrantAbortState *>(ctx);
  state->abort_result = abort_transaction(state->syscall_handler);
  state->abort_called = true;
  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

} // namespace

// Regression test for #2511: a mock callback calling abort_transaction() during
// its own root_invoke() used to finalize the running handler, so the resumed
// root frame returned success and a later try_commit_transaction() asserted.
// The reentrant abort is now rejected, the root invocation still succeeds, and
// the handler stays usable.
TEST(TestBindingsTest, ReentrantAbortDuringRootInvokeRejected)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ReentrantAbortState state;
  ContractAddress deployment_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_reentrant_abort, &state);

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

  auto result = root_invoke(syscall_handler, 0);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(result.serialized_output);
  free_ffi_data_buffer(result.serialized_trace);
  free_ffi_data_buffer(result.serialized_events);

  // The reentrant abort during the running root frame was rejected.
  EXPECT_TRUE(state.abort_called);
  EXPECT_EQ(state.abort_result, kTestFFIErrorCodeUnrecoverableError);

  // The handler was not corrupted: it can now be aborted and closed normally.
  EXPECT_EQ(abort_transaction(syscall_handler), kTestFFIErrorCodeNone);
  EXPECT_EQ(close_syscall_handler(syscall_handler), kTestFFIErrorCodeNone);
}

} // namespace hogd
