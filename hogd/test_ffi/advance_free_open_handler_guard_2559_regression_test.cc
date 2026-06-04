// Regression test for issue #2559. See test_ffi_test_helpers.h.
//
// advance_new_block() and free_db() reject the operation while a syscall
// handler derived from the same DB is still open, and they log that rejection
// before the function's try/catch boundary. Those helpers are noexcept and the
// log call allocates, so the diagnostic is now best-effort (wrapped so a
// logging failure cannot cross the noexcept boundary and terminate an honest
// caller). This test drives the guards with error-level logging enabled and
// confirms they return the structured error and that the DB stays usable after
// the handler closes. The logging-allocation crash itself needs a global
// allocator hook, which we deliberately do not install here.

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "types.hh"

namespace hogd {

// With error-level logging enabled, advance_new_block() and free_db() reject an
// operation while a handler is open (returning the structured error rather than
// crashing in the guard diagnostic), and both succeed once the handler is
// closed (issue #2559).
TEST(TestBindingsTest, AdvanceAndFreeOpenHandlerGuardsReturnStructuredError)
{
  enable_syscall_logging(kTestFFILogLevelError);

  auto *db = make_db();
  ASSERT_NE(db, nullptr);

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(handler.syscall_handler, nullptr);

  // Both guards reject while the handler borrows the DB.
  EXPECT_EQ(advance_new_block(db), kTestFFIErrorCodeUnrecoverableError);
  EXPECT_EQ(free_db(db), kTestFFIErrorCodeUnrecoverableError);

  // Closing the handler releases the borrow; both operations then succeed.
  ASSERT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
  EXPECT_EQ(advance_new_block(db), kTestFFIErrorCodeNone);
  EXPECT_EQ(close_runtime_context(rc), kTestFFIErrorCodeNone);
  EXPECT_EQ(free_db(db), kTestFFIErrorCodeNone);
}

} // namespace hogd
