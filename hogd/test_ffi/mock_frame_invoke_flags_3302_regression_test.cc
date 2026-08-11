// Regression test for issue #3302: test_ffi mock invocation frames accepted
// undefined InvokeFlag bits.
//
// Production SystemCallHandler::invoke() opens with
// CHECK(validate_invoke_flags(flags)), which admits only TryInvoke | ReadOnly
// (DelegateCall is defined but not yet implemented). start_mock_invocation_frame()
// stored the caller's flags verbatim, so a mock frame could carry bits that
// cannot exist on any production frame -- inert today, since only the TryInvoke
// and ReadOnly bits are ever read back, but a trap for whenever DelegateCall or
// a new flag lands. The mock pathway now validates against the same predicate
// and reports kTestFFIErrorCodeInvalidArgument instead of aborting the way
// production's CHECK does, since the point of the FFI error codes is to hand an
// honest caller a structured failure.

#include <array>
#include <cstdint>
#include <limits>
#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "invoke_flag.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

namespace {

Invocation
test_invocation()
{
  return Invocation{
      .invoked_address = ContractAddress{0xA0},
      .entrypoint = entrypoint_from_method_id(0x1111),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
}

} // namespace

// Every flag value production's validate_invoke_flags() rejects must be
// rejected here too, with a structured error rather than an abort.
TEST(TestBindingsTest, MockFrameOpenRejectsUndefinedInvokeFlagBits)
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

  auto invocation = test_invocation();

  constexpr std::array<int32_t, 6> kRejectedFlags = {
      // The bit from the issue report: not any defined InvokeFlag.
      0x40,
      // Defined in invoke_flag.x but explicitly not implemented, so production
      // does not admit it either.
      std::to_underlying(InvokeFlag::DelegateCall),
      // A valid bit does not launder an invalid one.
      std::to_underlying(InvokeFlag::TryInvoke) |
          std::to_underlying(InvokeFlag::DelegateCall),
      std::to_underlying(InvokeFlag::ReadOnly) | 0x40,
      // InvokeFlagSet is a signed int over the FFI boundary.
      -1,
      std::numeric_limits<int32_t>::min(),
  };

  for (int32_t flags : kRejectedFlags) {
    EXPECT_EQ(open_frame(handler.syscall_handler, invocation, flags),
              kTestFFIErrorCodeInvalidArgument)
        << "flags = " << flags;
  }

  // The rejection happens before any state is touched, so no frame was pushed:
  // finish still reports the empty-stack case (#2589) rather than finalizing a
  // frame the rejected opens left behind. That guard also returns before
  // mutating anything, so the handler stays usable below.
  auto no_frame_finish =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(no_frame_finish.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);
  free_ffi_data_buffer(no_frame_finish.serialized_output);
  free_ffi_data_buffer(no_frame_finish.serialized_trace);
  free_ffi_data_buffer(no_frame_finish.serialized_events);

  // A well-formed open on the same handler still works, and the frame finishes
  // and closes cleanly: the rejected opens perturbed neither the invocation
  // frame stack nor the paired mock-runtime and trace stacks.
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation,
                       std::to_underlying(InvokeFlag::None)),
            kTestFFIErrorCodeNone);

  auto finish =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(finish.serialized_output);
  free_ffi_data_buffer(finish.serialized_trace);
  free_ffi_data_buffer(finish.serialized_events);

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

// The flag set production admits must keep working: the new check narrows the
// mock pathway to production's set, it does not narrow it further.
TEST(TestBindingsTest, MockFrameOpenAcceptsProductionValidInvokeFlags)
{
  constexpr std::array<int32_t, 4> kAcceptedFlags = {
      std::to_underlying(InvokeFlag::None),
      std::to_underlying(InvokeFlag::TryInvoke),
      std::to_underlying(InvokeFlag::ReadOnly),
      std::to_underlying(InvokeFlag::TryInvoke) |
          std::to_underlying(InvokeFlag::ReadOnly),
  };

  for (int32_t flags : kAcceptedFlags) {
    // A fresh handler per flag value: finishing the root frame makes the
    // handler terminal, so these cannot share one.
    auto *db = make_db();
    ASSERT_NE(db, nullptr);
    x23::Defer d([db] { free_db(db); });

    auto *rc = make_runtime_context();
    ASSERT_NE(rc, nullptr);
    x23::Defer d2([rc] { close_runtime_context(rc); });

    auto handler = make_mocked_handler(db, rc);
    ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
    ASSERT_NE(handler.syscall_handler, nullptr);

    auto invocation = test_invocation();
    EXPECT_EQ(open_frame(handler.syscall_handler, invocation, flags),
              kTestFFIErrorCodeNone)
        << "flags = " << flags;

    auto finish =
        finish_mock_invocation_frame(handler.syscall_handler,
                                     std::to_underlying(InvokeErrorCode::None));
    EXPECT_EQ(finish.ffi_error_code, kTestFFIErrorCodeNone) << "flags = "
                                                            << flags;
    free_ffi_data_buffer(finish.serialized_output);
    free_ffi_data_buffer(finish.serialized_trace);
    free_ffi_data_buffer(finish.serialized_events);

    EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
              kTestFFIErrorCodeNone)
        << "flags = " << flags;
  }
}

} // namespace hogd
