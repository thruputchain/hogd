// Regression test for issue #3300. See test_ffi_test_helpers.h.
//
// SystemCallHandler::invoke() -- the production path that opens a child
// invocation frame -- inherits the parent frame's ReadOnly bit before starting
// the child:
//
//   if (!invocation_frame_stack_.empty() && is_read_only()) {
//     flags |= std::to_underlying(InvokeFlag::ReadOnly);
//   }
//
// start_mock_invocation_frame() stored the caller-supplied InvokeFlagSet
// verbatim and skipped that step, so a mock child opened with flags = 0
// underneath a ReadOnly parent was NOT read-only: is_read_only() reported false
// on it, wrap_after_parse()'s read-only gate let every non-read_only syscall
// through, and state-modifying syscalls production rejects with
// kSyscallErrorCodeInvalidSyscallInput produced real state changes -- a
// kLogEvent in such a child returned 0 and actually emitted an event under a
// read-only ancestor.
//
// The fix ORs InvokeFlag::ReadOnly into the flags handed to start_frame() when
// a frame is already open and is read-only, mirroring production. These tests
// drive the exported test_ffi C API: a child of a read-only parent is clamped
// (and stays clamped after the read-only ancestor is left behind on the stack),
// while a child of a writable parent is untouched.

#include <cstdint>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/contract_trait.h" // entrypoint_from_method_id
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "invoke_flag.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

namespace {

// Run the kLogEvent syscall (not read_only: in protocol/syscalls.yaml, so it is
// subject to wrap_after_parse()'s read-only gate) on the active frame of the
// currently pushed context. The three UINT64_MAX arguments are the null
// sentinels for the optional topics.
ForeignHostFnResult
run_log_event(std::string_view event_data)
{
  return mock_syscall_entrypoint(std::to_underlying(SyscallNo::kLogEvent),
                                 UINT64_MAX, UINT64_MAX, UINT64_MAX,
                                 reinterpret_cast<uint64_t>(event_data.data()),
                                 event_data.size(), 0, 0);
}

Invocation
make_invocation(ContractAddress const &address, uint64_t method_id)
{
  return Invocation{
      .invoked_address = address,
      .entrypoint = entrypoint_from_method_id(method_id),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
}

} // namespace

// #3300: a mock child frame opened with no flags beneath a ReadOnly parent
// inherits ReadOnly, so a state-modifying syscall is rejected there exactly as
// it is in the parent and emits no event.
TEST(TestBindingsTest, MockChildFrameInheritsParentReadOnly)
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
  auto *context = static_cast<SyscallHandlerContext *>(handler.syscall_handler);

  auto parent_invocation = make_invocation(ContractAddress{0xA0}, 0x1111);
  ASSERT_EQ(open_frame(handler.syscall_handler, parent_invocation,
                       std::to_underlying(InvokeFlag::ReadOnly)),
            kTestFFIErrorCodeNone);

  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  // The parent frame carries ReadOnly explicitly, so kLogEvent is gated there.
  EXPECT_TRUE(context->to_handler().is_read_only());
  auto parent_log = run_log_event("parent_event");
  EXPECT_EQ(parent_log.panic,
            std::to_underlying(HostFnErrorCode::NoneOrRecoverable));
  EXPECT_EQ(parent_log.result_code,
            std::to_underlying(
                SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput));

  // A child opened with flags = 0 must still be read-only: production ORs the
  // parent's ReadOnly bit in, so the mock path has to as well (#3300).
  auto child_invocation = make_invocation(ContractAddress{0xB0}, 0x2222);
  ASSERT_EQ(open_frame(handler.syscall_handler, child_invocation, /*flags=*/0),
            kTestFFIErrorCodeNone);
  EXPECT_TRUE(context->to_handler().is_read_only());

  auto child_log = run_log_event("child_event");
  EXPECT_EQ(child_log.panic,
            std::to_underlying(HostFnErrorCode::NoneOrRecoverable));
  EXPECT_EQ(child_log.result_code,
            std::to_underlying(
                SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput));

  // A grandchild, also opened with flags = 0, inherits through the clamped
  // child: the bit is now on the child frame itself, so the inheritance step
  // keeps holding once the originally-flagged ancestor is no longer the active
  // frame.
  auto grandchild_invocation = make_invocation(ContractAddress{0xC0}, 0x3333);
  ASSERT_EQ(open_frame(handler.syscall_handler, grandchild_invocation,
                       /*flags=*/0),
            kTestFFIErrorCodeNone);
  EXPECT_TRUE(context->to_handler().is_read_only());

  auto grandchild_log = run_log_event("grandchild_event");
  EXPECT_EQ(grandchild_log.panic,
            std::to_underlying(HostFnErrorCode::NoneOrRecoverable));
  EXPECT_EQ(grandchild_log.result_code,
            std::to_underlying(
                SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput));

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);

  // No frame emitted an event: every kLogEvent was rejected before mutating
  // state, so each finish comes back with a null event buffer. (Before the fix
  // the child and grandchild finishes returned a non-null serialized_events.)
  for (int i = 0; i < 3; ++i) {
    auto finish =
        finish_mock_invocation_frame(handler.syscall_handler,
                                     std::to_underlying(InvokeErrorCode::None));
    EXPECT_EQ(finish.ffi_error_code, kTestFFIErrorCodeNone);
    EXPECT_EQ(finish.serialized_events.data, nullptr);
    EXPECT_EQ(finish.serialized_events.len, 0U);
    free_ffi_data_buffer(finish.serialized_output);
    free_ffi_data_buffer(finish.serialized_trace);
    free_ffi_data_buffer(finish.serialized_events);
  }

  EXPECT_TRUE(context->can_be_closed());
  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

// The inheritance step must not clamp anything else: a child opened with no
// flags beneath a writable parent stays writable, so kLogEvent still succeeds
// there and its event is handed back on finish.
TEST(TestBindingsTest, MockChildFrameUnderWritableParentStaysWritable)
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
  auto *context = static_cast<SyscallHandlerContext *>(handler.syscall_handler);

  auto parent_invocation = make_invocation(ContractAddress{0xA1}, 0x1111);
  ASSERT_EQ(open_frame(handler.syscall_handler, parent_invocation, /*flags=*/0),
            kTestFFIErrorCodeNone);

  auto child_invocation = make_invocation(ContractAddress{0xB1}, 0x2222);
  ASSERT_EQ(open_frame(handler.syscall_handler, child_invocation, /*flags=*/0),
            kTestFFIErrorCodeNone);

  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);
  EXPECT_FALSE(context->to_handler().is_read_only());
  auto child_log = run_log_event("child_event");
  EXPECT_EQ(child_log.panic,
            std::to_underlying(HostFnErrorCode::NoneOrRecoverable));
  EXPECT_EQ(child_log.result_code,
            std::to_underlying(SyscallErrorCode::kSyscallErrorCodeNone));
  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);

  // The child succeeded, so its event survives to the caller.
  auto child_finish =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(child_finish.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_NE(child_finish.serialized_events.data, nullptr);
  EXPECT_GT(child_finish.serialized_events.len, 0U);
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

  EXPECT_TRUE(context->can_be_closed());
  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

} // namespace hogd
