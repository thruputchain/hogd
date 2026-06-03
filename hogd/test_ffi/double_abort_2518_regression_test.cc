// Regression test for issue #2518. See test_ffi_test_helpers.h.

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "x23/cleanup.h"

namespace hogd {

// Regression test for #2518: calling abort_transaction() twice on the same
// still-live mocked handler used to re-enter SystemCallHandler::abort_
// transaction(), which asserts !transaction_committed_or_aborted_ and so
// terminated assertion-enabled builds. The wrapper now consults its terminal
// flag and rejects the second abort with a structured error, leaving the
// handle closeable.
TEST(TestBindingsTest, DoubleAbortTransactionReturnsErrorInsteadOfAsserting)
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

  // The first abort finalizes the transaction.
  EXPECT_EQ(abort_transaction(handler.syscall_handler), kTestFFIErrorCodeNone);

  // The second abort is rejected with a structured error rather than tripping
  // the core single-use finalization assert.
  EXPECT_EQ(abort_transaction(handler.syscall_handler),
            kTestFFIErrorCodeUnrecoverableError);

  // The handle is still in a clean terminal state and closes normally.
  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

} // namespace hogd
