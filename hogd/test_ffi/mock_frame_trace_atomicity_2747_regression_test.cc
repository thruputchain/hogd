// Regression test for issue #2747 (and its amendment, finding 002). See
// test_ffi_test_helpers.h.
//
// SyscallHandlerContext::start_mock_invocation_frame() synthesizes the parent's
// kInvoke syscall trace record by hand, because the mock frame path enters at
// SystemCallHandler::start_frame(), below the wrap_after_parse() bracket that
// gives production its guarantee: start_syscall_trace(), a noexcept lambda,
// then finish_syscall_trace() -- so every record production opens is closed on
// every path.
//
// The mock path had no such bracket: it closed the record on exactly one of its
// exits, the admission refusal. Two others left it open.
//
//   * A std::bad_alloc from the throwing work after the append -- the
//     SyscallInvokeInputs copy that owns the child's variable-length calldata
//     (finding 002), the clamped Invocation copy that owns it again, the
//     EmptyRuntime preallocation and the two stack reserve() calls (the
//     originally reported failure point) -- unwound into
//     open_mock_invocation_frame()'s catch, which handed the caller a
//     structured unrecoverable FFI error.
//   * The start_frame() admission-failure return (#3058) returned the same
//     error directly, with no allocation failure in this function at all.
//
// Either way the appended record stayed behind carrying the sentinels
// start_syscall_trace() writes for finish_syscall_trace() to overwrite
// (gas_remaining_at_end == GasAmount max, status Unrecoverable, null outputs),
// and finishing the parent serialized a child Invoke that never opened a frame.
//
// The fix routes every exit through one self-disarming helper: a Defer
// completes the record if the stack unwinds, the two structured-error returns
// complete it with the more specific status they have, and the success path
// disarms once start_frame() hands responsibility to
// finish_mock_invocation_frame(). Finishing twice is what aborts (#2948), so
// the helper -- not the call sites -- owns whether a finish is still owed.
//
// The allocation failures themselves can only be forced with a global allocator
// hook, which this suite deliberately does not install (see
// noexcept_frame_trace_2644_2646_2741_regression_test.cc and
// trace_syscall_logging_2561_2562_regression_test.cc). What is checkable here
// is the invariant the fix establishes, on every failure return an honest
// caller can reach deterministically: after a refused open, the parent's trace
// holds no record still carrying the start-time sentinels.

#include <cstddef>
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
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/serialize.h"

namespace hogd {
namespace {

constexpr ContractAddress kParent{0xA0};
constexpr ContractAddress kChild{0xB0};
constexpr GasAmount kParentGas = 10'000'000;
constexpr GasAmount kChildGas = 5'000'000;

Invocation
make_invocation(ContractAddress address, uint64_t method, GasAmount gas_limit)
{
  return Invocation{
      .invoked_address = address,
      .entrypoint = entrypoint_from_method_id(method),
      .gas_limit = gas_limit,
      .calldata = {},
  };
}

// Finish the innermost open mock frame and hand the buffers to a scope guard.
// Returns the deserialized trace.
InvocationFrameTrace
finish_frame_and_take_trace(void *handler)
{
  auto res =
      finish_mock_invocation_frame(handler,
                                   std::to_underlying(InvokeErrorCode::None));
  x23::Defer free_buffers([&] {
    free_ffi_data_buffer(res.serialized_output);
    free_ffi_data_buffer(res.serialized_trace);
    free_ffi_data_buffer(res.serialized_events);
  });
  EXPECT_EQ(res.ffi_error_code, kTestFFIErrorCodeNone);

  InvocationFrameTrace trace;
  if (res.ffi_error_code != kTestFFIErrorCodeNone ||
      res.serialized_trace.data == nullptr) {
    ADD_FAILURE() << "expected a serialized trace from the parent finish";
    return trace;
  }
  x23::deserialize(&trace, x23::BufView{res.serialized_trace.data,
                                        res.serialized_trace.len});
  return trace;
}

// The exact evidence #2747 reported: a record start_syscall_trace() appended
// and finish_syscall_trace() never overwrote. Both sentinels are checked, since
// a *finished* record may legitimately carry an Unrecoverable status, but no
// finished record can carry GasAmount max as its end gas -- the parent frames
// here run on a bounded budget, and finish_syscall_trace() CHECKs that it is
// overwriting the sentinel exactly once.
void
expect_no_unfinished_records(InvocationFrameTrace const &trace)
{
  for (auto const &call : trace.system_calls) {
    EXPECT_NE(call.gas_remaining_at_end, std::numeric_limits<GasAmount>::max())
        << "syscall " << std::to_underlying(call.body.syscall_number())
        << " was traced but never finished";
    EXPECT_LE(call.gas_remaining_at_end, call.gas_remaining_at_start);
  }
}

} // namespace

// A refused open is the deterministic instance of the failure shape: the record
// is appended, the open returns an error, and the parent frame keeps running.
// The record must be complete by the time the caller sees the error.
TEST(TestBindingsTest, RefusedMockFrameOpenLeavesNoUnfinishedInvokeTrace)
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
  // Close through a guard: an unfinished frame left by a failed assertion
  // blocks free_db() on the open-handler guard and hangs the binary instead of
  // reporting the failure.
  x23::Defer d3([&] {
    EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
              kTestFFIErrorCodeNone);
  });

  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(kParent, 0x1111, kParentGas)),
            kTestFFIErrorCodeNone);

  // Reentrance (#3301): refused by validate_new_invocation().
  EXPECT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(kParent, 0x2222, kChildGas)),
            kTestFFIErrorCodeInvalidArgument);

  // Undefined flag bits (#3302): the other validate_new_invocation() refusal.
  EXPECT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(kChild, 0x3333, kChildGas), 0x40),
            kTestFFIErrorCodeInvalidArgument);

  auto trace = finish_frame_and_take_trace(handler.syscall_handler);

  // Both refusals were traced, and both records are complete.
  ASSERT_EQ(trace.system_calls.size(), 2U);
  expect_no_unfinished_records(trace);

  auto const &reentrance = trace.system_calls[0];
  EXPECT_EQ(reentrance.body.syscall_number(), SyscallNo::kInvoke);
  EXPECT_EQ(reentrance.status.error(), HostFnErrorCode::NoneOrRecoverable);
  EXPECT_EQ(reentrance.status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeReentrance);
  EXPECT_EQ(reentrance.gas_remaining_at_end, kParentGas);
  EXPECT_EQ(reentrance.body.invoke().outputs, nullptr);

  auto const &bad_flags = trace.system_calls[1];
  EXPECT_EQ(bad_flags.body.syscall_number(), SyscallNo::kInvoke);
  EXPECT_EQ(bad_flags.status.error(), HostFnErrorCode::NoneOrRecoverable);
  EXPECT_EQ(bad_flags.status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
  EXPECT_EQ(bad_flags.gas_remaining_at_end, kParentGas);
  EXPECT_EQ(bad_flags.body.invoke().outputs, nullptr);
}

// An open that fails before start_mock_invocation_frame() runs at all -- the
// serialized invocation does not parse -- must not touch the parent's trace.
// This pins the other half of the ordering: nothing is appended speculatively.
TEST(TestBindingsTest, UnparseableMockFrameOpenAppendsNoInvokeTrace)
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
  x23::Defer d3([&] {
    EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
              kTestFFIErrorCodeNone);
  });

  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(kParent, 0x1111, kParentGas)),
            kTestFFIErrorCodeNone);

  // A truncated serialization of a well-formed Invocation: deserialize() throws
  // x23::XdrException, which open_mock_invocation_frame() reports without ever
  // reaching the trace append.
  auto serialized = x23::serialize(make_invocation(kChild, 0x2222, kChildGas));
  ASSERT_GT(serialized.size(), 4U);
  FFISerializedInvocation truncated = {
      .data = serialized.data(),
      .len = static_cast<uint32_t>(serialized.size() - 4),
  };
  EXPECT_EQ(open_mock_invocation_frame(handler.syscall_handler, truncated, 0),
            kTestFFIErrorCodeDeserializeError);

  auto trace = finish_frame_and_take_trace(handler.syscall_handler);
  EXPECT_TRUE(trace.system_calls.empty());
  expect_no_unfinished_records(trace);
}

// The success path still records what production records: the *requested* gas
// limit and flags (captured before the parent-budget clamp and the ReadOnly
// inheritance), and a child trace attached once the child frame finishes. The
// reorder moved the input copy earlier; it must not have changed what is
// copied.
TEST(TestBindingsTest, MockFrameOpenStillTracesRequestedInvokeInputs)
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
  x23::Defer d3([&] {
    EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
              kTestFFIErrorCodeNone);
  });

  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(kParent, 0x1111, kParentGas)),
            kTestFFIErrorCodeNone);

  auto child = make_invocation(kChild, 0x2222, kChildGas);
  child.calldata.resize(64);
  for (std::size_t i = 0; i < child.calldata.size(); ++i) {
    child.calldata[i] = static_cast<uint8_t>(i);
  }
  ASSERT_EQ(open_frame(handler.syscall_handler, child,
                       std::to_underlying(InvokeFlag::TryInvoke)),
            kTestFFIErrorCodeNone);

  auto child_finish =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  free_ffi_data_buffer(child_finish.serialized_output);
  free_ffi_data_buffer(child_finish.serialized_trace);
  free_ffi_data_buffer(child_finish.serialized_events);
  ASSERT_EQ(child_finish.ffi_error_code, kTestFFIErrorCodeNone);

  auto trace = finish_frame_and_take_trace(handler.syscall_handler);
  ASSERT_EQ(trace.system_calls.size(), 1U);
  expect_no_unfinished_records(trace);

  auto const &call = trace.system_calls[0];
  EXPECT_EQ(call.body.syscall_number(), SyscallNo::kInvoke);
  EXPECT_EQ(call.status.error(), HostFnErrorCode::NoneOrRecoverable);
  EXPECT_EQ(call.gas_remaining_at_start, kParentGas);

  auto const &invoke = call.body.invoke();
  EXPECT_EQ(invoke.inputs.address, kChild);
  EXPECT_EQ(invoke.inputs.calldata, child.calldata);
  EXPECT_EQ(invoke.inputs.inputs.entrypoint_id,
            sdk_method_id(child.entrypoint));
  EXPECT_EQ(invoke.inputs.inputs.gas_limit, kChildGas);
  EXPECT_EQ(invoke.inputs.inputs.flags,
            InvokeFlagSet{std::to_underlying(InvokeFlag::TryInvoke)});
  // A child frame ran, so its trace is attached.
  EXPECT_NE(invoke.outputs, nullptr);
}

} // namespace hogd
