// Regression test for issue #2596. See test_ffi_test_helpers.h.
//
// close_syscall_handler() takes ownership of the handler and aborts its
// still-open transaction. If that implicit abort fails, the function returns
// kTestFFIErrorCodeUnrecoverableError -- but the handler is still destroyed,
// which drops the DBContext open-handler guard even though the HogDb InvokeTX
// may not have been closed. advance_new_block() then saw has_open_handlers()
// == false and proceeded into the scheduler over orphaned transaction state.
//
// The abort failure is reachable in honest use: advancing the DB block while a
// transaction is outstanding commits/closes that transaction's block, so a
// later abort of it fails. This test drives exactly that by advancing the
// underlying DBContext block out from under the open handler (the DBContext
// method, not the guarded FFI wrapper), then closing the handler. The fix
// poisons the DB on failed close so advance_new_block() refuses to run rather
// than driving the scheduler with a possibly-open InvokeTX; free_db() stays the
// safe teardown path.

#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "types.hh"

namespace hogd {

TEST(TestBindingsTest, FailedCloseAfterBlockAdvancePoisonsDb)
{
  auto genesis_root = test_genesis_root_contract_bytes();
  auto db_res = create_db(nullptr, 0, NetworkHash{}.data(), genesis_root.data(),
                          genesis_root.size());
  ASSERT_EQ(db_res.ffi_error_code, kTestFFIErrorCodeNone);
  auto *db = db_res.db;
  auto *typed_db = static_cast<DBContext *>(db);
  // free_db() is the one lifecycle op that stays valid on a poisoned DB.
  x23::Defer free_db_defer(
      [db] { EXPECT_EQ(free_db(db), kTestFFIErrorCodeNone); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer close_rc_defer([rc] { close_runtime_context(rc); });

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(handler.syscall_handler, nullptr);
  // The open-handler guard is up for the whole handler lifetime (#2572).
  ASSERT_TRUE(typed_db->has_open_handlers());

  // Advance the underlying block out from under the open handler, committing
  // the handler's transaction's block so the close-time abort below fails.
  typed_db->advance_new_block();

  // The implicit abort now fails; close returns unrecoverable and drops the
  // guard, but the fix poisons the DB lifecycle (issue #2596).
  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeUnrecoverableError);
  EXPECT_FALSE(typed_db->has_open_handlers());
  EXPECT_TRUE(typed_db->is_lifecycle_poisoned());

  // With the guard gone, only the poison stops advance_new_block() from driving
  // the scheduler over the orphaned InvokeTX. It must be rejected.
  EXPECT_EQ(advance_new_block(db), kTestFFIErrorCodeUnrecoverableError);
}

} // namespace hogd
