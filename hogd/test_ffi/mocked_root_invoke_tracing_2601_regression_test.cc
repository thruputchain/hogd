// Regression test for issue #2601. See test_ffi_test_helpers.h.
//
// root_invoke(handler, enable_tracing=1) on a mocked handler must be rejected
// with kTestFFIErrorCodeUnrecoverableError. Previously the C binding enabled
// tracing *before* delegating to the wrapper that rejects mocked handlers, and
// SystemCallHandler::enable_tracing() asserts the invocation-frame stack is
// empty. With a mock invocation frame already open the assert fired (aborting
// Debug builds) before the mocked-handler guard could return its structured
// error. enable_tracing is now applied inside SyscallHandlerContext::
// root_invoke(), after the mocked-handler guard, so the rejection wins.

#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include "auth_claim.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {

TEST(TestBindingsTest, MockedRootInvokeWithTracingRejectsInsteadOfAsserting)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  // Open a mocked handler with tracing DISABLED so the only enable_tracing()
  // call comes from the root_invoke() below.
  AuthClaim auth_claim_data(AuthClaimType::kSourceAddressAuthData);
  auto serialized_auth = x23::serialize(auth_claim_data);
  FFISerializedAuthClaimData ffi_auth = {
      .data = serialized_auth.data(),
      .len = static_cast<uint32_t>(serialized_auth.size()),
  };
  SHA256Hash source_tx_hash{};
  auto handler =
      open_mocked_syscall_handler(db, rc, ffi_auth, source_tx_hash.data(),
                                  /*enable_tracing=*/0);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(handler.syscall_handler, nullptr);

  // Open a mock invocation frame so the SystemCallHandler's invocation-frame
  // stack is non-empty (the precondition that used to trip enable_tracing()).
  Invocation invocation = {
      .invoked_address = ContractAddress{0xA0},
      .entrypoint = entrypoint_from_method_id(0x1111),
      .gas_limit = 10'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);

  // Request tracing on the mocked handler: the mocked-handler guard must return
  // a structured error rather than asserting in enable_tracing() (issue #2601).
  auto res = root_invoke(handler.syscall_handler, /*enable_tracing=*/1);
  EXPECT_EQ(res.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);
  free_ffi_data_buffer(res.serialized_output);
  free_ffi_data_buffer(res.serialized_trace);
  free_ffi_data_buffer(res.serialized_events);

  // The handler is unharmed: finish the open frame and close cleanly.
  auto finish =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(finish.serialized_output);
  free_ffi_data_buffer(finish.serialized_trace);
  free_ffi_data_buffer(finish.serialized_events);

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

} // namespace hogd
