// Regression test for issue #3301. See test_ffi_test_helpers.h.
//
// start_mock_invocation_frame() enters at SystemCallHandler::start_frame(),
// below the reentrance check invoke() makes, so it used to admit a frame for an
// address that already had one -- a stack production cannot build, since
// invoke() refuses that kInvoke with kSyscallErrorCodeReentrance. Both paths
// now share validate_new_invocation(), and neither offers a way to opt out
// (#3346 removed the per-test bypass).

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

// A second frame for a live address is refused, and the refusal is reported the
// way production reports one: the caller's synthetic kInvoke carries
// kSyscallErrorCodeReentrance, with its gas unchanged and no child trace.
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
  ASSERT_EQ(open_frame(handler.syscall_handler, make_invocation(kAddr, 0x1111)),
            kTestFFIErrorCodeNone);

  // Same address, parent still active.
  EXPECT_EQ(open_frame(handler.syscall_handler, make_invocation(kAddr, 0x2222)),
            kTestFFIErrorCodeInvalidArgument);

  auto parent_finish = finish_frame_ok(handler.syscall_handler);
  x23::Defer free_parent([&] { free_finish_result(parent_finish); });
  ASSERT_NE(parent_finish.serialized_trace.data, nullptr);

  InvocationFrameTrace trace;
  x23::deserialize(&trace, x23::BufView{parent_finish.serialized_trace.data,
                                        parent_finish.serialized_trace.len});

  // The refused invoke is still traced, as production traces a refused syscall.
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

// A rejected open advances none of the three stacks it would have pushed
// (invocation frames, mock runtimes, stashed traces), so the caller owes no
// finish for it. Advancing one would make the finishes below mismatch.
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

  // Exactly two finishes are owed; a third would fault on an empty stack
  // (#2589).
  auto child_finish = finish_frame_ok(handler.syscall_handler);
  free_finish_result(child_finish);
  auto parent_finish = finish_frame_ok(handler.syscall_handler);
  free_finish_result(parent_finish);

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

// The whole stack is scanned, so a grandparent's address is reentrant too.
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

// The rule is about *active* frames: once a child finishes, its address is free
// again, so sequential sibling calls to one contract still work.
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
