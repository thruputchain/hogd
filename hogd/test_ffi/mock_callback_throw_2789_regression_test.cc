// Regression test for issue #2789.
//
// ForeignEntryPoint (a mock contract's callback) is an ordinary C function
// pointer: nothing declares it noexcept, and nothing stops an honest harness
// caller's callback from throwing. MockContract::invoke() called it directly
// and let the throw unwind, so whether the caller got a structured FFI error or
// a dead process depended entirely on which path reached the callback:
//
//   * the root mock frame unwound into hogd::root_invoke()'s catch, which
//     translated the throw into kTestFFIErrorCodeUnrecoverableError (#2693),
//     but
//   * a child Invoke reached the callback from syscall_Invoke_inner(...)()
//     noexcept -- below the Wasmtime host import, the RV32 ecall trampoline,
//     and mock_syscall_entrypoint()'s own catch alike -- so the throw hit that
//     noexcept boundary and called std::terminate().
//
// The exception boundary now lives in MockContract::invoke(), the single place
// the callback is called, so every path gets the same structured Unrecoverable
// result regardless of what sits above it. The tests below drive the child
// Invoke through mock_syscall_entrypoint(kInvoke, ...), the same
// syscall_Invoke_inner()() body a Wasmtime or RV32 parent reaches through its
// trampoline, and cover the variants reported on the issue: a std::exception
// throw, a non-std::exception throw, and a throw under InvokeFlag::TryInvoke
// (where the process died before the parent could read frame_status).

#include <cstdint>
#include <stdexcept>
#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/test_ffi/mock_contract.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
namespace {

// What the parent observed from its child Invoke syscall, recorded by the
// parent callback so the test can assert on it after root_invoke() returns.
struct ChildInvokeObservation {
  bool syscall_ran = false;
  int32_t panic = 0;
  int32_t result_code = 0;
};

// Context handed to the parent mock callback: which child to invoke, with
// which flags, and where to record what came back.
struct ParentContext {
  ContractAddress child_addr;
  InvokeFlagSet flags;
  ChildInvokeObservation observation;
};

// Parent mock contract: issues one child Invoke syscall through the public
// mock syscall entrypoint (the same syscall_Invoke_inner()() body a Wasmtime or
// RV32 parent reaches) and records the result rather than interpreting it, so
// the test can assert on the raw host-function status.
ForeignInvokeResult
parent_invokes_child(void *foreign_context, uint64_t /*method_number*/)
{
  auto *ctx = static_cast<ParentContext *>(foreign_context);

  CopyableInvokeInputs inputs{
      .entrypoint_id = 0x1234,
      .gas_limit = UINT64_MAX,
      .flags = ctx->flags,
  };
  CopyableInvokeOutputs outputs{};
  Calldata empty_calldata;

  auto result = mock_syscall_entrypoint(
      std::to_underlying(SyscallNo::kInvoke),
      reinterpret_cast<uint64_t>(ctx->child_addr.data()),
      reinterpret_cast<uint64_t>(empty_calldata.data()), empty_calldata.size(),
      0, 0, reinterpret_cast<uint64_t>(&inputs),
      reinterpret_cast<uint64_t>(&outputs));

  ctx->observation = ChildInvokeObservation{
      .syscall_ran = true,
      .panic = result.panic,
      .result_code = result.result_code,
  };

  // Propagate the child's failure, as a real guest parent does implicitly: an
  // Unrecoverable host-function error traps a Wasmtime or RV32 parent's
  // execution. A mock parent has no guest execution to trap, so it declares
  // the same outcome itself.
  return ForeignInvokeResult{
      .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
}

// Child mock contract whose callback throws a std::exception (issue #2789).
ForeignInvokeResult
child_throws_std_exception(void * /*unused*/, uint64_t /*method_number*/)
{
  throw std::runtime_error("mock child callback throws");
}

// Child mock contract whose callback throws a non-std::exception. Callbacks
// are not restricted to std::exception, so the catch(...) arm has to cover
// this too (the #2693 gap, here on the nested path).
ForeignInvokeResult
child_throws_non_std(void * /*unused*/, uint64_t /*method_number*/)
{
  // NOLINTNEXTLINE(hicpp-exception-baseclass): the point is a non-std throw.
  throw 7;
}

// Callbacks that return rather than throw, to pin that the catch changes
// nothing for them.
ForeignInvokeResult
child_returns_none(void * /*unused*/, uint64_t /*method_number*/)
{
  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

ForeignInvokeResult
child_returns_deterministic(void * /*unused*/, uint64_t /*method_number*/)
{
  return ForeignInvokeResult{
      .panic = std::to_underlying(InvokeErrorCode::Deterministic)};
}

// Parent + throwing child deployed on a real (non-mocked) handler, ready for
// root_invoke().
struct InvokeHarness {
  void *db = nullptr;
  void *rc = nullptr;
  void *syscall_handler = nullptr;
};

InvokeHarness
setup(ParentContext *parent_ctx, ForeignEntryPoint child_entry_point)
{
  InvokeHarness h;
  h.db = make_db();
  EXPECT_NE(h.db, nullptr);
  h.rc = make_runtime_context();
  EXPECT_NE(h.rc, nullptr);

  ContractAddress parent_addr{};
  parent_ctx->child_addr = ContractAddress{};
  parent_ctx->child_addr[0] = 1;

  deploy_mock_contract(h.db, h.rc, /*mock_id=*/0, parent_invokes_child,
                       parent_ctx, parent_addr);
  EXPECT_EQ(setup_grant_contract_storage_tokens(h.db, parent_addr.data(),
                                                1'000'000'000),
            kTestFFIErrorCodeNone);

  deploy_mock_contract(h.db, h.rc, /*mock_id=*/1, child_entry_point, nullptr,
                       parent_ctx->child_addr);
  EXPECT_EQ(setup_grant_contract_storage_tokens(h.db,
                                                parent_ctx->child_addr.data(),
                                                1'000'000'000),
            kTestFFIErrorCodeNone);

  EXPECT_EQ(advance_new_block(h.db), kTestFFIErrorCodeNone);

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = parent_addr,
                     .entrypoint = entrypoint_from_method_id(0),
                     .gas_limit = UINT64_MAX,
                     .calldata = {},
                 },
             .uniquifier = {.block_expiry_height = INT64_MAX, .nonce = 0}},
      .signatures = {},
  };
  h.syscall_handler = make_syscall_handler(h.db, h.rc, stx);
  EXPECT_NE(h.syscall_handler, nullptr);
  return h;
}

void
teardown(InvokeHarness &h)
{
  EXPECT_EQ(close_syscall_handler(h.syscall_handler), kTestFFIErrorCodeNone);
  EXPECT_EQ(close_runtime_context(h.rc), kTestFFIErrorCodeNone);
  EXPECT_EQ(free_db(h.db), kTestFFIErrorCodeNone);
}

// Run a parent that invokes a throwing child, and check both what the parent
// saw from the syscall and what the caller got back from root_invoke().
void
expect_child_throw_is_unrecoverable(ForeignEntryPoint child_entry_point,
                                    InvokeFlagSet flags)
{
  ParentContext ctx{.child_addr = {}, .flags = flags, .observation = {}};
  auto h = setup(&ctx, child_entry_point);

  auto result = root_invoke(h.syscall_handler, /*enable_tracing=*/0);

  // The throw must not terminate the process before the syscall returns: the
  // parent runs to completion and sees an Unrecoverable host-function error
  // for the child frame, which is the structured result the callback could
  // have returned on its own.
  EXPECT_TRUE(ctx.observation.syscall_ran);
  EXPECT_EQ(ctx.observation.panic,
            std::to_underlying(HostFnErrorCode::Unrecoverable));

  // An Unrecoverable child ends the whole transaction, so the caller gets a
  // structured FFI error rather than a crashed process -- the same outcome the
  // root mock path already produced through root_invoke()'s catch (#2693).
  // TryInvoke does not contain it either: containment covers a child that
  // faulted deterministically, not a system failure.
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);
  free_ffi_data_buffer(result.serialized_output);
  free_ffi_data_buffer(result.serialized_trace);
  free_ffi_data_buffer(result.serialized_events);

  // The handler is still closeable: the frame failed through the normal error
  // path, so every step that a stack unwind would have skipped still ran --
  // finish_frame() and the mock-runtime stack pop of #2470 among them.
  teardown(h);
}

} // namespace

// The boundary itself, exercised where it lives. MockContract::invoke() needs
// neither a DB nor an open frame, so this pins the translation directly: a
// throwing callback becomes Unrecoverable, and a returning callback is
// untouched.
TEST(TestBindingsTest, MockContractInvokeTranslatesCallbackThrows)
{
  // MockContract::invoke() reports an escaped callback through the process
  // logger, which the FFI installs on first use. Nothing here goes through an
  // FFI entry point, so install it explicitly rather than logging through a
  // null logger (issue #2478); enable_syscall_logging() runs that init.
  enable_syscall_logging(kTestFFILogLevelError);

  auto invoke_with = [](ForeignEntryPoint entry_point) {
    return MockContract(entry_point, nullptr).invoke(/*method_id=*/0x1234);
  };

  auto std_throw = invoke_with(child_throws_std_exception);
  ASSERT_FALSE(std_throw.has_value());
  EXPECT_EQ(std_throw.error().code(), InvokeErrorCode::Unrecoverable);

  auto non_std_throw = invoke_with(child_throws_non_std);
  ASSERT_FALSE(non_std_throw.has_value());
  EXPECT_EQ(non_std_throw.error().code(), InvokeErrorCode::Unrecoverable);

  // A callback that returns is unaffected: success stays success, and a
  // declared panic code is still reported as itself rather than being
  // flattened into Unrecoverable.
  EXPECT_TRUE(invoke_with(child_returns_none).has_value());

  auto deterministic = invoke_with(child_returns_deterministic);
  ASSERT_FALSE(deterministic.has_value());
  EXPECT_EQ(deterministic.error().code(), InvokeErrorCode::Deterministic);
}

// The reported case: a Wasmtime/RV32 parent's nested Invoke to a child mock
// callback that throws std::runtime_error terminated the process instead of
// returning kTestFFIErrorCodeUnrecoverableError (#2789).
TEST(TestBindingsTest, NestedInvokeStdExceptionCallbackThrowIsUnrecoverable)
{
  expect_child_throw_is_unrecoverable(child_throws_std_exception,
                                      InvokeFlagSet{});
}

// The same path with a non-std::exception throw: nothing in the callback
// contract restricts what may be thrown, so catch(...) has to cover it.
TEST(TestBindingsTest, NestedInvokeNonStdCallbackThrowIsUnrecoverable)
{
  expect_child_throw_is_unrecoverable(child_throws_non_std, InvokeFlagSet{});
}

// TryInvoke variant: the process died before syscall_Invoke_inner()() could
// write CopyableInvokeOutputs.frame_status, so the containment the flag
// promises never happened. The parent now returns from the syscall with a
// structured host error instead.
TEST(TestBindingsTest, TryInvokeCallbackThrowIsUnrecoverable)
{
  expect_child_throw_is_unrecoverable(child_throws_std_exception,
                                      std::to_underlying(
                                          InvokeFlag::TryInvoke));
}

} // namespace hogd
