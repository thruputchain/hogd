// Regression test for issue #2784. See test_ffi_test_helpers.h.

#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

// Regression test for #2784: close_syscall_handler() used to delete a handler
// that was still installed in the thread-local context slot by push_context().
// can_be_closed() only checked the mock-runtime stack, so the close succeeded
// and freed the handler while current_context_ still pointed at it; a later
// pop_context() then dereferenced the freed SyscallHandlerContext (ASan
// heap-use-after-free). close_syscall_handler() now refuses to close a handler
// that is still the current thread's pushed context, so the caller must pop it
// first -- preserving balanced push/pop ownership -- after which the handler
// closes cleanly.
TEST(TestBindingsTest, ClosePushedMockContextRejectedNotDangling)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer db_cleanup([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer rc_cleanup([rc] { close_runtime_context(rc); });

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(handler.syscall_handler, nullptr);

  Invocation invocation = {
      .invoked_address = ContractAddress{},
      .entrypoint = ContractEntrypointID{},
      .gas_limit = 0,
      .calldata = {},
  };

  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  ASSERT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer output_cleanup(
      [&] { free_ffi_data_buffer(finish_res.serialized_output); });
  x23::Defer trace_cleanup(
      [&] { free_ffi_data_buffer(finish_res.serialized_trace); });
  x23::Defer events_cleanup(
      [&] { free_ffi_data_buffer(finish_res.serialized_events); });

  ASSERT_EQ(abort_transaction(handler.syscall_handler), kTestFFIErrorCodeNone);

  // The handler is still installed in the thread-local context slot, so closing
  // it would leave current_context_ dangling. Reject the close with a
  // structured error rather than freeing the still-published handler.
  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeUnrecoverableError);

  // The handler is still live: balanced cleanup pops the context, then the
  // close succeeds. (Under ASan this also confirms no use-after-free and no
  // leak.)
  EXPECT_EQ(pop_context(), kTestFFIErrorCodeNone);
  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);

  // The context slot is empty after the balanced pop.
  EXPECT_EQ(pop_context(), kTestFFIErrorCodeUnrecoverableError);
}

} // namespace hogd
