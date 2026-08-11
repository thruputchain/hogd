// Regression test for issue #3301. See test_ffi_test_helpers.h.
//
// A manually-opened mock invocation frame must be admitted under production's
// reentrance rule. SystemCallHandler::invoke() refuses a kInvoke whose address
// already has a frame on the stack, returning kSyscallErrorCodeReentrance
// before any child frame is created. start_mock_invocation_frame() reaches
// SystemCallHandler::start_frame() directly, below that check, so it used to
// admit the reentrant frame and let the harness build a frame stack production
// could never produce.
//
// Nothing sanctioned that gap. The per-test escape hatch invoke() used to
// consult (hack_dont_use_test_bypass_allow_reentrance) was opt-in, read only
// inside invoke(), and never set by the mocked FFI handler -- and it is gone
// entirely as of #3346, so neither pathway offers a way to ask for a reentrant
// stack now.
//
// The tests below pin the mock pathway to the production behavior:
//
//   1. ReentrantMockFrameRejectedAndTraced: opening a second frame for an
//      address that already has one is rejected with a structured FFI error, and
//      the parent's synthetic kInvoke is traced as kSyscallErrorCodeReentrance
//      with the parent's gas untouched -- production charges nothing for an
//      invoke it refuses.
//   2. ReentranceGuardRejectedOpenLeavesFrameStackUsable: the rejection is a
//      no-op. No frame was pushed, so the caller owes no matching finish: the
//      parent is still the innermost frame, a distinct-address child still opens
//      on top of it, and the handler closes cleanly at the end.
//   3. ReentranceGuardChecksEntireStack: an address held by a grandparent is
//      rejected too, not just the innermost frame's -- invoke() scans the whole
//      stack.
//   4. FinishedFrameAddressIsReusable: the guard is about *active* frames. Once
//      a child has finished, its address is free again for a sibling frame.

#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/serialize.h"

namespace hogd {
namespace {

constexpr GasAmount kFrameGas = 10'000'000;

Invocation
make_invocation(ContractAddress address, uint64_t method,
                GasAmount gas_limit = kFrameGas)
{
  return Invocation{
      .invoked_address = address,
      .entrypoint = entrypoint_from_method_id(method),
      .gas_limit = gas_limit,
      .calldata = {},
  };
}

// Finish the innermost open mock frame, requiring a structured success, and
// hand back the result with its buffers still owned by the caller.
ForeignTracedInvokeResult
finish_frame_ok(void *handler)
{
  auto res =
      finish_mock_invocation_frame(handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(res.ffi_error_code, kTestFFIErrorCodeNone);
  return res;
}

void
free_finish_result(ForeignTracedInvokeResult const &res)
{
  free_ffi_data_buffer(res.serialized_output);
  free_ffi_data_buffer(res.serialized_trace);
  free_ffi_data_buffer(res.serialized_events);
}

} // namespace

// The core regression assertion. A frame is open for 0xA0; opening a second one
// for 0xA0 is refused, and the refusal is reported the way production reports
// it: the caller gets an error instead of a frame, and the parent's synthetic
// kInvoke carries kSyscallErrorCodeReentrance with the parent's gas unchanged
// and no child trace attached. Before the fix the second open returned
// kTestFFIErrorCodeNone.
TEST(TestBindingsTest, ReentrantMockFrameRejectedAndTraced)
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

  ContractAddress const kAddr{0xA0};
  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(kAddr, 0x1111)),
            kTestFFIErrorCodeNone);

  // Same address, parent still active: production's kInvoke would answer
  // kSyscallErrorCodeReentrance (code 4).
  EXPECT_EQ(open_frame(handler.syscall_handler, make_invocation(kAddr, 0x2222)),
            kTestFFIErrorCodeInvalidArgument);

  auto parent_finish = finish_frame_ok(handler.syscall_handler);
  x23::Defer free_parent([&] { free_finish_result(parent_finish); });
  ASSERT_NE(parent_finish.serialized_trace.data, nullptr);

  InvocationFrameTrace trace;
  x23::deserialize(&trace, x23::BufView{parent_finish.serialized_trace.data,
                                        parent_finish.serialized_trace.len});

  // The refused invoke is still recorded: production traces the syscall its
  // caller attempted, with the reentrance status.
  ASSERT_EQ(trace.system_calls.size(), 1U);
  auto const &invoke_trace = trace.system_calls[0];
  EXPECT_EQ(invoke_trace.body.syscall_number(), SyscallNo::kInvoke);
  EXPECT_EQ(invoke_trace.status.error(), HostFnErrorCode::NoneOrRecoverable);
  EXPECT_EQ(invoke_trace.status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeReentrance);

  // A refused invoke costs the caller nothing and produces no child frame.
  EXPECT_EQ(invoke_trace.gas_remaining_at_start, kFrameGas);
  EXPECT_EQ(invoke_trace.gas_remaining_at_end, kFrameGas);
  auto const &invoke_body = invoke_trace.body.invoke();
  EXPECT_EQ(invoke_body.inputs.address, kAddr);
  EXPECT_EQ(invoke_body.outputs, nullptr);

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

// The rejection must not disturb the frame stacks it declined to advance.
// start_mock_invocation_frame() pushes onto three stacks (the handler's
// invocation frames, the mock runtimes, and the stashed parent traces), and a
// rejected open pushes onto none of them -- so the caller owes no finish for the
// frame that never opened. If the reject path had advanced one stack, the
// mismatched finishes below would fault or the close would refuse.
TEST(TestBindingsTest, ReentranceGuardRejectedOpenLeavesFrameStackUsable)
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

  ContractAddress const kParent{0xA0};
  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(kParent, 0x1111)),
            kTestFFIErrorCodeNone);

  EXPECT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(kParent, 0x2222)),
            kTestFFIErrorCodeInvalidArgument);

  // The parent is still the innermost frame, so a distinct-address child opens
  // on top of it as usual.
  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(ContractAddress{0xB0}, 0x3333)),
            kTestFFIErrorCodeNone);

  // Exactly two finishes are owed: the child and the parent. A third would fault
  // on an empty mock runtime stack (#2589).
  auto child_finish = finish_frame_ok(handler.syscall_handler);
  free_finish_result(child_finish);
  auto parent_finish = finish_frame_ok(handler.syscall_handler);
  free_finish_result(parent_finish);

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

// invoke() scans the whole invocation stack, not just the innermost frame, so an
// address held by a grandparent is reentrant too.
TEST(TestBindingsTest, ReentranceGuardChecksEntireStack)
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

  ContractAddress const kRoot{0xA0};
  ASSERT_EQ(open_frame(handler.syscall_handler, make_invocation(kRoot, 0x1111)),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(ContractAddress{0xB0}, 0x2222)),
            kTestFFIErrorCodeNone);

  // 0xA0 is two frames down, not the active one.
  EXPECT_EQ(open_frame(handler.syscall_handler, make_invocation(kRoot, 0x3333)),
            kTestFFIErrorCodeInvalidArgument);

  auto child_finish = finish_frame_ok(handler.syscall_handler);
  free_finish_result(child_finish);
  auto root_finish = finish_frame_ok(handler.syscall_handler);
  free_finish_result(root_finish);

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

// The guard is about frames that are *active*, matching
// InvocationFrameStack::has_active_frame_for(). A finished frame's address is
// free again, so sequential sibling calls to the same contract -- which
// production admits -- keep working.
TEST(TestBindingsTest, FinishedFrameAddressIsReusable)
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

  ContractAddress const kChild{0xB0};
  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(ContractAddress{0xA0}, 0x1111)),
            kTestFFIErrorCodeNone);

  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(kChild, 0x2222)),
            kTestFFIErrorCodeNone);
  auto first_child = finish_frame_ok(handler.syscall_handler);
  free_finish_result(first_child);

  // The first child is gone from the stack, so the same address opens again.
  EXPECT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(kChild, 0x3333)),
            kTestFFIErrorCodeNone);
  auto second_child = finish_frame_ok(handler.syscall_handler);
  free_finish_result(second_child);

  auto root_finish = finish_frame_ok(handler.syscall_handler);
  free_finish_result(root_finish);

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

} // namespace hogd
