// Regression tests for the test_ffi result-handoff failure-atomicity cluster:
// issues #2658, #2699, #2730, and #2768. See test_ffi_test_helpers.h.
//
// finish_mock_invocation_frame() and root_invoke() finalize/commit a frame and
// then hand its serialized result buffers back to the caller. Each had a window
// where a failure during that handoff -- a thrown by-value copy or an
// allocation failure during FFI serialization -- escaped without leaving the
// handler in a consistent state, so an honest C API caller that received an
// unrecoverable FFI error could still drive a hidden commit, or corrupt the
// mock-frame lifecycle:
//
//   * #2768: finish_mock_invocation_frame() only poisoned the handler on a
//     handoff failure when the *root* frame had finished. A non-root child
//     whose output finish_frame() had already merged into its parent was not
//     poisoned, so the parent could later finish and try_commit_transaction()
//     returned ExecResultOK for a child result the caller never received.
//   * #2730: the finalized root output was copied by value
//     (get_most_recent_frame_output(), which copies a nonempty ReturnBuffer)
//     *before* the handoff-failure marker was installed, so a throw during that
//     copy escaped without poisoning the handler.
//   * #2699: root_invoke() copied the root frame's events
//     (copy_last_frame_events()) outside its guarded handoff path, so a throw
//     during that copy escaped without poisoning the handler.
//   * #2658: finish_mock_invocation_frame() popped the mock runtime *before*
//     snapshotting the active frame's events (copy_active_frame_events()), so a
//     throw during that copy left the mock runtime stack popped while the lower
//     handler frame was still active -- can_be_closed() reported an empty stack
//     while is_running() stayed true, and the finish could not be retried.
//
// The fix makes both paths failure-atomic: the event snapshot now happens
// before any wrapper-state mutation (#2658), and a single scope-exit guard
// poisons the handler on ANY handoff failure after the frame is committed --
// thrown copies included, and unconditionally for non-root frames
// (#2699/#2730/#2768).
//
// The failure branches can only be forced with a global allocator hook, which
// (matching the other setup/finish/trace regression tests in this directory) we
// deliberately do not install. These tests pin the success-path invariants the
// fix must preserve: a normal nested/root finish and root_invoke still hand
// back their result buffers and commit, and a frame that emitted events still
// finishes and closes cleanly.

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/contract_trait.h" // entrypoint_from_method_id
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// #2768: a non-root child frame whose output is handed back successfully must
// merge into its parent and let the parent finish and commit normally. (Under
// allocation failure the handoff is now poisoned, blocking the commit; on the
// success path here it must not be.)
TEST(TestBindingsTest, NonRootChildFinishHandsOffOutputThenParentCommits)
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

  Invocation parent_invocation = {
      .invoked_address = ContractAddress{0xA1},
      .entrypoint = entrypoint_from_method_id(0x1111),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, parent_invocation),
            kTestFFIErrorCodeNone);

  Invocation child_invocation = {
      .invoked_address = ContractAddress{0xB2},
      .entrypoint = entrypoint_from_method_id(0x2222),
      .gas_limit = 500'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, child_invocation),
            kTestFFIErrorCodeNone);

  // Give the child a nonempty return buffer so finishing it exercises the
  // output copy/handoff path (#2730's copy lives on the same path).
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);
  SyscallHandlerContext::get_context()->to_handler().set_return_buffer(
      ReturnBuffer{0xCA, 0xFE, 0xBA, 0xBE}, /*revert_frame=*/false);
  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);

  auto child_finish =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  ASSERT_EQ(child_finish.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_NE(child_finish.serialized_output.data, nullptr);
  EXPECT_GT(child_finish.serialized_output.len, 0U);
  free_ffi_data_buffer(child_finish.serialized_output);
  free_ffi_data_buffer(child_finish.serialized_trace);
  free_ffi_data_buffer(child_finish.serialized_events);

  auto parent_finish =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  ASSERT_EQ(parent_finish.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(parent_finish.serialized_output);
  free_ffi_data_buffer(parent_finish.serialized_trace);
  free_ffi_data_buffer(parent_finish.serialized_events);

  // The child output was handed back, so commit proceeds normally.
  auto commit = try_commit_transaction(handler.syscall_handler);
  free_ffi_data_buffer(commit.serialized_conflict);
  EXPECT_EQ(commit.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_EQ(commit.result_code, std::to_underlying(ExecResult::ExecResultOK));
  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

// #2730: a root mock frame with a nonempty return buffer finishes, hands its
// finalized output back, and commits. The poison guard now wraps the output
// copy, but a successful handoff must leave the handler committable.
TEST(TestBindingsTest, RootMockFinishWithReturnBufferHandsOffOutputThenCommits)
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

  Invocation invocation = {
      .invoked_address = ContractAddress{0xC0},
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);

  std::vector<std::byte> return_buffer(256, std::byte{0x5A});
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);
  auto return_res =
      mock_syscall_entrypoint(std::to_underlying(SyscallNo::kReturn),
                              reinterpret_cast<uint64_t>(return_buffer.data()),
                              return_buffer.size(), 0, 0, 0, 0, 0);
  EXPECT_EQ(return_res.panic, std::to_underlying(HostFnErrorCode::Return));
  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);

  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  ASSERT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_NE(finish_res.serialized_output.data, nullptr);
  EXPECT_GT(finish_res.serialized_output.len, 0U);
  free_ffi_data_buffer(finish_res.serialized_output);
  free_ffi_data_buffer(finish_res.serialized_trace);
  free_ffi_data_buffer(finish_res.serialized_events);

  auto commit = try_commit_transaction(handler.syscall_handler);
  free_ffi_data_buffer(commit.serialized_conflict);
  EXPECT_EQ(commit.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_EQ(commit.result_code, std::to_underlying(ExecResult::ExecResultOK));
  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

// #2658: a mock frame that emitted an event finishes cleanly and leaves the
// handler closeable. The event snapshot now precedes the mock-runtime pop, so a
// successful finish drains both stacks in lockstep (no stranded runtime).
TEST(TestBindingsTest, MockFrameWithEventFinishesAndCloses)
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

  Invocation invocation = {
      .invoked_address = ContractAddress{0xCB},
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);

  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);
  std::string_view frame_event = "frame_event";
  auto log_res =
      mock_syscall_entrypoint(std::to_underlying(SyscallNo::kLogEvent),
                              UINT64_MAX, UINT64_MAX, UINT64_MAX,
                              reinterpret_cast<uint64_t>(frame_event.data()),
                              frame_event.size(), 0, 0);
  ASSERT_EQ(log_res.panic,
            std::to_underlying(HostFnErrorCode::NoneOrRecoverable));
  ASSERT_EQ(log_res.result_code,
            std::to_underlying(SyscallErrorCode::kSyscallErrorCodeNone));
  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);

  auto *context = static_cast<SyscallHandlerContext *>(handler.syscall_handler);
  ASSERT_TRUE(context->to_handler().is_running());
  ASSERT_FALSE(context->can_be_closed());

  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  ASSERT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(finish_res.serialized_output);
  free_ffi_data_buffer(finish_res.serialized_trace);
  free_ffi_data_buffer(finish_res.serialized_events);

  // The frame finished, so both the mock runtime stack and the lower frame are
  // drained: the handler is closeable.
  EXPECT_TRUE(context->can_be_closed());
  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

// #2699: a non-mocked root invocation of an event-emitting contract returns its
// serialized events to the caller and commits. The poison guard now wraps the
// event-vector copy, but a successful handoff must leave the result
// committable.
TEST(TestBindingsTest, RootInvokeWithEventHandsOffEventsThenCommits)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ContractAddress deployment_addr =
      deploy_mock_contract(db, rc, /*mock_id=*/1, mock_contract_create_event,
                           nullptr);
  ASSERT_EQ(setup_grant_contract_storage_tokens(db, deployment_addr.data(),
                                                1'000'000),
            kTestFFIErrorCodeNone);

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = deployment_addr,
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 },
             .uniquifier = {.block_expiry_height = INT64_MAX, .nonce = 0}},
      .signatures = {},
  };

  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  ASSERT_NE(syscall_handler, nullptr);
  x23::Defer d3([syscall_handler] { close_syscall_handler(syscall_handler); });

  auto result = root_invoke(syscall_handler, 0);
  ASSERT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_NE(result.serialized_events.data, nullptr);
  EXPECT_GT(result.serialized_events.len, 0U);
  free_ffi_data_buffer(result.serialized_output);
  free_ffi_data_buffer(result.serialized_trace);
  free_ffi_data_buffer(result.serialized_events);

  auto commit = try_commit_transaction(syscall_handler);
  free_ffi_data_buffer(commit.serialized_conflict);
  EXPECT_EQ(commit.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_EQ(commit.result_code, std::to_underlying(ExecResult::ExecResultOK));
}

} // namespace hogd
