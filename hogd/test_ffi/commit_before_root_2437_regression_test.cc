// Regression test for issue #2437. See test_ffi_test_helpers.h.

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

// Regression test for #2437: try_commit_transaction() before root_invoke() used
// to trip the internal assert(last_frame_.has_value()) and abort the process.
// It must now return a structured FFI error.
TEST(TestBindingsTest, CommitBeforeRootInvokeReturnsErrorInsteadOfAborting)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = ContractAddress{},
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 }},
      .signatures = {},
  };

  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  ASSERT_NE(syscall_handler, nullptr);

  auto commit = try_commit_transaction(syscall_handler);
  free_ffi_data_buffer(commit.serialized_conflict);
  EXPECT_EQ(commit.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);

  // The handler was never committed, so it can still be aborted and closed.
  EXPECT_EQ(abort_transaction(syscall_handler), kTestFFIErrorCodeNone);
  EXPECT_EQ(close_syscall_handler(syscall_handler), kTestFFIErrorCodeNone);
}

} // namespace hogd
