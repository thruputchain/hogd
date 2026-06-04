// Regression test for issue #2510. See test_ffi_test_helpers.h.

#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {
namespace {

struct ReentrantPopState {
  TestFFIErrorCode pop_result = kTestFFIErrorCodeNone;
  bool pop_called = false;
};

// A mock root contract callback that tries to pop the thread-local context the
// running root_invoke() installed and owns.
ForeignInvokeResult
mock_contract_reentrant_pop_context(void *ctx, uint64_t method_number)
{
  if (method_number != 0x1234) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }
  auto *state = static_cast<ReentrantPopState *>(ctx);
  state->pop_result = pop_context();
  state->pop_called = true;
  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

} // namespace

// Regression test for #2510: a mock callback calling the exported pop_context()
// during root_invoke() used to clear the root-owned TLS context, after which
// root_invoke()'s deferred pop_context() aborted on CHECK(get_context() ==
// this). pop_context() now refuses to pop the root-owned context.
TEST(TestBindingsTest, ReentrantPopContextDuringRootInvokeRejected)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ReentrantPopState state;
  ContractAddress deployment_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_reentrant_pop_context,
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

  auto result = root_invoke(syscall_handler, 0);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(result.serialized_output);
  free_ffi_data_buffer(result.serialized_trace);
  free_ffi_data_buffer(result.serialized_events);

  // The reentrant pop_context() during the running root frame was rejected.
  EXPECT_TRUE(state.pop_called);
  EXPECT_EQ(state.pop_result, kTestFFIErrorCodeUnrecoverableError);

  // The handler's context bookkeeping is intact and it closes cleanly.
  EXPECT_EQ(close_syscall_handler(syscall_handler), kTestFFIErrorCodeNone);
}

} // namespace hogd
