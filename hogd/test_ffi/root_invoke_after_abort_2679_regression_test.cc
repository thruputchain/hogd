// Regression test for issue #2679: test_ffi root_invoke() re-enters after abort.
//
// After exported abort_transaction() makes a handler terminal, a later
// root_invoke() on the same still-live handle must be rejected with a structured
// FFI error rather than running a fresh event-emitting root frame whose pending
// events make exported close_syscall_handler() terminate the process in debug
// builds.

#include <cstdint>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

TEST(TestBindingsTest, RootInvokeAfterAbortRejectedWithStructuredError)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer free_db_on_exit([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer close_rc_on_exit([rc] { close_runtime_context(rc); });

  ContractAddress deployment_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_create_event, nullptr);
  ASSERT_EQ(setup_grant_contract_storage_tokens(db, deployment_addr.data(),
                                                1'000'000),
            kTestFFIErrorCodeNone);

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

  ASSERT_EQ(abort_transaction(syscall_handler), kTestFFIErrorCodeNone);

  // Post-abort root_invoke() must be rejected up front with null buffers.
  auto result = root_invoke(syscall_handler, 0);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);
  EXPECT_EQ(result.serialized_output.data, nullptr);
  EXPECT_EQ(result.serialized_trace.data, nullptr);
  EXPECT_EQ(result.serialized_events.data, nullptr);

  // The handler stays cleanly closeable: no post-abort root frame left pending
  // events behind, so close does not trip the debug destructor.
  EXPECT_EQ(close_syscall_handler(syscall_handler), kTestFFIErrorCodeNone);
}

} // namespace hogd
