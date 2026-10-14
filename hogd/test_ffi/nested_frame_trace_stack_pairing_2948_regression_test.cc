// Regression test for issue #2948 (hogd#3184). See test_ffi_test_helpers.h.
//
// SyscallHandlerContext keeps invocation_frame_trace_stack_ paired with the
// handler's invocation-frame depth: start_mock_invocation_frame() stashes the
// parent InvocationFrameTrace that handler_.start_frame() hands back, and
// finish_mock_invocation_frame() restores it when the frame finishes.
//
// The stash used to be pushed only when start_frame() returned a non-null
// trace. That stayed LIFO-consistent as long as the one frame that never
// stashes is the root (finished last), but a best-effort trace drop
// (start_syscall_trace() clearing the active trace after an allocation failure,
// #2644) makes a *non-root* start_frame() return null too. The missing push
// then landed mid-stack, so a deeper frame's finish popped and restored an
// ancestor's saved trace one frame early, and the next finish re-finalized the
// same parent Invoke syscall record -- aborting on the gas_remaining_at_end
// guard in finish_syscall_trace(). start_mock_invocation_frame() now pushes one
// slot (a null placeholder when there is nothing to stash) per mock frame, so
// each finish restores exactly the trace its own frame stashed.
//
// The abort itself only triggers under the mid-stack trace drop, which requires
// a global allocator hook that -- matching mock_frame_nested_2560 and the
// noexcept_frame_trace_2644 cluster -- we deliberately omit. This test pins the
// success-path invariant the fix must not break: opening three nested traced
// frames and finishing them grandchild-then-child-then-parent keeps the
// frame/trace stacks paired, finalizes each Invoke record exactly once, and
// leaves the handler cleanly closeable with a serialized trace at every level.

#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {
namespace {

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

// Finish one mock frame and require a structured (non-aborting) result. The
// bug's symptom is a process abort in finish_syscall_trace(), so simply
// returning here with ffi_error_code == None is the core regression signal.
ForeignTracedInvokeResult
finish_frame_ok(void *handler)
{
  auto finish =
      finish_mock_invocation_frame(handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish.ffi_error_code, kTestFFIErrorCodeNone);
  return finish;
}

void
free_traced_result(ForeignTracedInvokeResult result)
{
  free_ffi_data_buffer(result.serialized_output);
  free_ffi_data_buffer(result.serialized_trace);
  free_ffi_data_buffer(result.serialized_events);
}

} // namespace

// Open parent -> child -> grandchild mock frames on a traced handler, then
// finish grandchild-then-child-then-parent. Each nested open records an Invoke
// trace entry on its parent and stashes the parent trace; the paired restore on
// finish must line up with the frame that stashed it, so no finish re-finalizes
// an ancestor's Invoke record.
TEST(TestBindingsTest, NestedTracedMockFramesRestoreParentTraceInLockstep)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer free_db_on_return([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer close_rc_on_return([rc] { close_runtime_context(rc); });

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(handler.syscall_handler, nullptr);

  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(ContractAddress{0xA0}, 0x1111,
                                       10'000'000)),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(ContractAddress{0xB0}, 0x2222,
                                       5'000'000)),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(open_frame(handler.syscall_handler,
                       make_invocation(ContractAddress{0xC0}, 0x3333,
                                       1'000'000)),
            kTestFFIErrorCodeNone);

  free_traced_result(finish_frame_ok(handler.syscall_handler)); // grandchild
  free_traced_result(finish_frame_ok(handler.syscall_handler)); // child

  // The root finish restores no ancestor trace; with tracing on it serializes
  // the fully-attached root trace.
  auto root_finish = finish_frame_ok(handler.syscall_handler);
  EXPECT_NE(root_finish.serialized_trace.data, nullptr);
  EXPECT_GT(root_finish.serialized_trace.len, 0U);
  free_traced_result(root_finish);

  // All three stacks drained in lockstep, so the handler closes cleanly.
  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

} // namespace hogd
