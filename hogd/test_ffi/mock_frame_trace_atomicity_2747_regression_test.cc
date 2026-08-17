// Regression test for issue #2747 (and its amendment, finding 002). See
// test_ffi_test_helpers.h.
//
// start_mock_invocation_frame() left its synthetic kInvoke record unfinished on
// every exit but one, so the parent's finish serialized a child Invoke that
// never opened a frame.
//
// Forcing the allocation failures needs a global allocator hook, which this
// suite does not install (see
// noexcept_frame_trace_2644_2646_2741_regression_test.cc). These cover the
// exits reachable without one: no record is left carrying the start-time
// sentinels.

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

// Finish the innermost open mock frame and return its deserialized trace.
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

// End gas is the sentinel to test: a finished record may carry an Unrecoverable
// status, but not GasAmount max, since these parents run on a bounded budget.
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

// A refusal appends the record, returns an error, and leaves the parent
// running -- the record must be complete by the time the caller sees the error.
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
  // Guarded: a frame left open by a failed assertion hangs free_db().
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

// A failure before start_mock_invocation_frame() runs must append nothing.
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

  // Truncated: deserialize() throws before the trace append is reached.
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

// The success path disarms, and still traces the *requested* gas limit and
// flags -- captured before the clamp and the ReadOnly inheritance.
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
