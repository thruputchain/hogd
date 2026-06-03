// Regression test for issue #2454. See test_ffi_test_helpers.h.

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

// Regression test for #2454: closing a handler after a successful
// event-emitting root_invoke() without committing used to terminate debug
// builds in ~SystemCallHandler() ("destroyed with events to emit") because the
// implicit abort never drained the root frame's events. The close must now
// succeed.
TEST(TestBindingsTest, CloseAfterEventRootInvokeWithoutCommitSucceeds)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ContractAddress deployment_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_create_event, nullptr);

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = deployment_addr,
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 }},
      .signatures = {},
  };

  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  ASSERT_NE(syscall_handler, nullptr);

  auto result = root_invoke(syscall_handler, 0);
  ASSERT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(result.serialized_events.data, nullptr);
  ASSERT_GT(result.serialized_events.len, 0U);
  free_ffi_data_buffer(result.serialized_output);
  free_ffi_data_buffer(result.serialized_events);
  free_ffi_data_buffer(result.serialized_trace);

  // Close without committing: the implicit abort discards the pending events.
  EXPECT_EQ(close_syscall_handler(syscall_handler), kTestFFIErrorCodeNone);
}

} // namespace hogd
