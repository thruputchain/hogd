// Regression test for issue #2680: test_ffi finish_mock_invocation_frame (and
// open_mock_invocation_frame) finalize after abort.
//
// After exported abort_transaction() makes a mocked handler terminal, the mock
// frame open/finish paths must reject the terminal handler with a structured FFI
// error instead of starting/finishing a fresh root mock frame whose pending
// events make exported close_syscall_handler() terminate the process in debug
// builds.

#include <cstdint>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

TEST(TestBindingsTest, MockFrameOpenAndFinishAfterAbortRejected)
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

  ASSERT_EQ(abort_transaction(handler.syscall_handler), kTestFFIErrorCodeNone);

  Invocation invocation = {
      .invoked_address = ContractAddress{},
      .entrypoint = entrypoint_from_method_id(0),
      .gas_limit = 1'000'000,
      .calldata = {},
  };

  // Opening a mock frame after abort must be rejected, so no post-abort root
  // frame can be finalized.
  EXPECT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeUnrecoverableError);

  // Finishing after abort must likewise be rejected with a structured error.
  auto finish =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);
  EXPECT_EQ(finish.serialized_output.data, nullptr);
  EXPECT_EQ(finish.serialized_events.data, nullptr);

  // No pending post-abort frame state, so close stays clean.
  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

} // namespace hogd
