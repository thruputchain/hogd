// Regression test for issue #2589. See test_ffi_test_helpers.h.
//
// finish_mock_invocation_frame() can be called on a fresh mocked handler before
// any mock invocation frame has been opened. That out-of-order but well-formed
// call used to reach pop_mock_runtime() with an empty mock_runtime_stack_,
// whose only empty-stack guard is a debug assert -- aborting Debug builds and
// reading std::vector::back() out of bounds in release. finish now rejects the
// no-open-frame case with a structured kTestFFIErrorCodeUnrecoverableError.

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

TEST(TestBindingsTest, FinishWithoutOpenFrameReturnsStructuredError)
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
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  // No mock invocation frame has been opened, so mock_runtime_stack_ is empty.
  // finish must reject with a structured error instead of popping an empty
  // stack (issue #2589).
  auto res = finish_mock_invocation_frame(
      handler.syscall_handler, std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(res.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);
  free_ffi_data_buffer(res.serialized_output);
  free_ffi_data_buffer(res.serialized_trace);
  free_ffi_data_buffer(res.serialized_events);
}

} // namespace hogd
