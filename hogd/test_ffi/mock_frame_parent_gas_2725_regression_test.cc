// Regression test for issue #2725. See test_ffi_test_helpers.h.
//
// A manually-opened mock invocation frame must be metered exactly the way
// production kInvoke meters a child frame. Production does this in two places:
//
//   * syscall_Invoke_inner() calls SystemCallHandler::invoke() with
//     min(requested gas_limit, remaining_gas), so a child frame is never
//     admitted with a bigger budget than its parent still has; and
//   * wrap_after_parse()'s gas epilogue rewrites a handleable syscall result to
//     OutOfGas whenever gas_consumed >= gas_remaining, so a child that spends
//     the parent's entire remaining budget surfaces as an out-of-gas Invoke.
//
// The mock pathway did neither. start_mock_invocation_frame() built the child's
// EmptyRuntime from the raw requested gas limit, and
// finish_mock_invocation_frame() debited the parent *after* the child frame had
// already been committed, discarding the boolean failure that
// MockGasRuntime::consume_gas() returns when the charge meets or exceeds the
// parent's remaining gas. A child that consumed exactly the parent's budget
// therefore finished as a successful Return whose synthetic parent kInvoke was
// traced as kSyscallErrorCodeNone, even though the parent's gas had just been
// zeroed by a debit that failed -- an out-of-gas classification bypass relative
// to the production pathway.
//
// The tests below pin the mock pathway to the production behavior:
//
//   1. ChildFrameGasIsClampedToParentRemaining: a child that asks for more
//      gas than the parent has left runs on the parent's remaining budget, so
//      a syscall costing exactly that budget is itself rewritten to OutOfGas
//      by wrap_after_parse() -- the same result the syscall would get under
//      production kInvoke.
//   2. ExactBudgetChildFinishesParentInvokeAsOutOfGas: when the child's total
//      gas meets the parent's remaining budget, the parent's synthetic kInvoke
//      is traced as OutOfGas with zero gas left, instead of as a success.
//   3. UnderBudgetChildFinishesParentInvokeAsSuccess: the ordinary case is
//      untouched -- a child that stays strictly under the parent's budget still
//      traces a successful kInvoke and leaves the parent the difference.

#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
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

// Gas a one-byte kSHA256 syscall costs. The tests size the parent frame around
// this so the child's spend lands exactly on, or just under, the parent budget.
constexpr GasAmount kShaGas = SHA256HashCost(1);

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

// Run one one-byte kSHA256 syscall against the innermost open mock frame. The
// caller must hold a pushed context.
ForeignHostFnResult
run_sha256_syscall()
{
  static uint8_t sha_input = 0x42;
  SHA256Hash sha_output = {};
  return mock_syscall_entrypoint(std::to_underlying(SyscallNo::kSHA256),
                                 reinterpret_cast<uint64_t>(&sha_input),
                                 sizeof(sha_input),
                                 reinterpret_cast<uint64_t>(sha_output.data()),
                                 sha_output.size(), 0, 0, 0);
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

// A child mock frame that asks for more gas than its parent has left runs on
// the parent's remaining budget, exactly as production kInvoke's
// min(gas_limit, remaining_gas) clamp arranges. The clamp is observable through
// the child's own syscall: kSHA256 costs precisely the clamped budget, so
// wrap_after_parse()'s gas epilogue rewrites it to OutOfGas. Before the fix the
// child ran on its full requested (parent_gas + 1) budget and the syscall
// succeeded.
TEST(TestBindingsTest, ChildFrameGasIsClampedToParentRemaining)
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

  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(ContractAddress{0xA0}, 0x1111, kShaGas)),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  // Ask for one more gas unit than the parent has. Production would hand the
  // child min(kShaGas + 1, kShaGas) == kShaGas.
  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(ContractAddress{0xB0}, 0x2222,
                                       kShaGas + 1)),
            kTestFFIErrorCodeNone);

  // The child was clamped to exactly kShaGas, so this syscall spends its whole
  // budget and comes back OutOfGas rather than succeeding.
  auto sha_res = run_sha256_syscall();
  EXPECT_EQ(sha_res.panic, std::to_underlying(HostFnErrorCode::OutOfGas));

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);

  auto child_finish = finish_frame_ok(handler.syscall_handler);
  x23::Defer free_child([&] { free_finish_result(child_finish); });
  EXPECT_EQ(child_finish.gas_consumed, kShaGas);

  auto parent_finish = finish_frame_ok(handler.syscall_handler);
  x23::Defer free_parent([&] { free_finish_result(parent_finish); });

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

// The child consumes exactly the parent's remaining gas, so the parent's debit
// meets its budget. Production classifies that as OutOfGas
// (gas_consumed >= gas_remaining in wrap_after_parse()); the synthetic parent
// kInvoke must be traced the same way. Before the fix the failed debit was
// discarded and the kInvoke was traced as kSyscallErrorCodeNone.
TEST(TestBindingsTest, ExactBudgetChildFinishesParentInvokeAsOutOfGas)
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

  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(ContractAddress{0xA0}, 0x1111, kShaGas)),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(ContractAddress{0xB0}, 0x2222,
                                       kShaGas + 1)),
            kTestFFIErrorCodeNone);

  // Whatever the syscall's own classification, it charges the child kShaGas --
  // exactly what the parent has left.
  (void)run_sha256_syscall();
  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);

  auto child_finish = finish_frame_ok(handler.syscall_handler);
  x23::Defer free_child([&] { free_finish_result(child_finish); });
  EXPECT_EQ(child_finish.gas_consumed, kShaGas);

  auto parent_finish = finish_frame_ok(handler.syscall_handler);
  x23::Defer free_parent([&] { free_finish_result(parent_finish); });
  ASSERT_NE(parent_finish.serialized_trace.data, nullptr);

  InvocationFrameTrace trace;
  x23::deserialize(&trace, x23::BufView{parent_finish.serialized_trace.data,
                                        parent_finish.serialized_trace.len});

  ASSERT_EQ(trace.system_calls.size(), 1U);
  auto const &invoke_trace = trace.system_calls[0];
  EXPECT_EQ(invoke_trace.body.syscall_number(), SyscallNo::kInvoke);
  EXPECT_EQ(invoke_trace.gas_remaining_at_start, kShaGas);
  EXPECT_EQ(invoke_trace.gas_remaining_at_end, 0U);

  // The core regression assertion: the parent could not pay for the child, so
  // its synthetic kInvoke is an out-of-gas host-function error, not a success.
  EXPECT_EQ(invoke_trace.status.error(), HostFnErrorCode::OutOfGas);

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

// The ordinary case must be unchanged: a child that stays strictly under the
// parent's budget traces a successful kInvoke and leaves the parent the
// remainder. This pins the fix to the exact/over-budget case rather than
// letting it classify every nested finish as out of gas.
TEST(TestBindingsTest, UnderBudgetChildFinishesParentInvokeAsSuccess)
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

  // One gas unit of headroom above what the child will spend.
  constexpr GasAmount kParentGas = kShaGas + 1;
  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(ContractAddress{0xA0}, 0x1111,
                                       kParentGas)),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(ContractAddress{0xB0}, 0x2222,
                                       kParentGas)),
            kTestFFIErrorCodeNone);

  // The child has kParentGas == kShaGas + 1, so the syscall succeeds.
  auto sha_res = run_sha256_syscall();
  EXPECT_EQ(sha_res.panic,
            std::to_underlying(HostFnErrorCode::NoneOrRecoverable));
  EXPECT_EQ(sha_res.result_code,
            std::to_underlying(SyscallErrorCode::kSyscallErrorCodeNone));
  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);

  auto child_finish = finish_frame_ok(handler.syscall_handler);
  x23::Defer free_child([&] { free_finish_result(child_finish); });
  EXPECT_EQ(child_finish.gas_consumed, kShaGas);

  InvocationFrameOutput child_output;
  x23::deserialize(&child_output,
                   x23::BufView{child_finish.serialized_output.data,
                                child_finish.serialized_output.len});
  EXPECT_EQ(child_output.status_code(), InvocationFrameStatusCode::Return);

  auto parent_finish = finish_frame_ok(handler.syscall_handler);
  x23::Defer free_parent([&] { free_finish_result(parent_finish); });
  ASSERT_NE(parent_finish.serialized_trace.data, nullptr);

  InvocationFrameTrace trace;
  x23::deserialize(&trace, x23::BufView{parent_finish.serialized_trace.data,
                                        parent_finish.serialized_trace.len});

  ASSERT_EQ(trace.system_calls.size(), 1U);
  auto const &invoke_trace = trace.system_calls[0];
  EXPECT_EQ(invoke_trace.body.syscall_number(), SyscallNo::kInvoke);
  EXPECT_EQ(invoke_trace.gas_remaining_at_start, kParentGas);
  EXPECT_EQ(invoke_trace.gas_remaining_at_end, kParentGas - kShaGas);
  EXPECT_EQ(invoke_trace.status.error(), HostFnErrorCode::NoneOrRecoverable);
  EXPECT_EQ(invoke_trace.status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeNone);

  auto const &invoke_body = invoke_trace.body.invoke();
  // The requested (unclamped) limit is what production records in the syscall
  // trace inputs, before invoke()'s min() clamp.
  EXPECT_EQ(invoke_body.inputs.inputs.gas_limit, kParentGas);
  ASSERT_NE(invoke_body.outputs, nullptr);
  ASSERT_NE(invoke_body.outputs->child_trace, nullptr);
  EXPECT_EQ(invoke_body.outputs->child_trace->status.status_code(),
            InvocationFrameStatusCode::Return);

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

} // namespace hogd
