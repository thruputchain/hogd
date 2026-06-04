// Regression test for issue #2560. See test_ffi_test_helpers.h.
//
// open_mock_invocation_frame() opens a child mock frame by advancing the lower
// invocation-frame stack and the paired mock-runtime/trace stacks.
// start_mock_invocation_frame() now performs its throwing allocations before
// the (noexcept) handler_.start_frame() publish step (#2507), so a failure can
// no longer leave the lower frame stack advanced while the mock stacks lag
// behind
// -- the half-open frame that #2560 reported, which later aborted in
// finish_syscall_trace(). The abort only triggers under allocation failure (a
// global allocator hook we deliberately omit); this test pins the success-path
// invariant by manually opening a parent and a nested child frame with tracing
// on, finishing both, and confirming the handler closes cleanly with a
// consistent trace.

#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// Manually open a parent mock frame and a nested child mock frame on a traced
// handler, then finish child-then-parent. The frame/mock-runtime/trace stacks
// stay paired (#2507/#2560), so each finish succeeds and the handler closes
// cleanly afterward.
TEST(TestBindingsTest, NestedMockFrameOpenFinishKeepsHandlerCloseable)
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

  Invocation parent_invocation = {
      .invoked_address = ContractAddress{0xA0},
      .entrypoint = entrypoint_from_method_id(0x1111),
      .gas_limit = 10'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, parent_invocation),
            kTestFFIErrorCodeNone);

  Invocation child_invocation = {
      .invoked_address = ContractAddress{0xB0},
      .entrypoint = entrypoint_from_method_id(0x2222),
      .gas_limit = 5'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, child_invocation),
            kTestFFIErrorCodeNone);

  // Finish the child frame, then the parent (root) frame.
  auto child_finish =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(child_finish.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(child_finish.serialized_output);
  free_ffi_data_buffer(child_finish.serialized_trace);
  free_ffi_data_buffer(child_finish.serialized_events);

  auto parent_finish =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(parent_finish.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(parent_finish.serialized_output);
  free_ffi_data_buffer(parent_finish.serialized_trace);
  free_ffi_data_buffer(parent_finish.serialized_events);

  // Both stacks drained in lockstep, so the handler closes cleanly.
  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

} // namespace hogd
