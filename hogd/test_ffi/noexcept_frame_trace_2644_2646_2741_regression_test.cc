// Regression test for the "frame/syscall trace allocation terminates a traced
// test FFI invocation" cluster: issues #2644, #2646, and #2741. See
// test_ffi_test_helpers.h.
//
// All three live on the noexcept frame-lifecycle path that runs underneath the
// exported test FFI boundary, where a std::bad_alloc from best-effort trace
// bookkeeping used to escape and std::terminate() the process:
//
//   * #2644: SystemCallHandler::start_syscall_trace() appends a SyscallTrace
//     record to InvocationFrameTrace::system_calls before the noexcept syscall
//     body in wrap_after_parse(); on the real WASM path the syscall dispatches
//     through wrap_outer() noexcept, so the unguarded push_back terminated the
//     process. It now drops the frame's trace and continues untraced.
//   * #2646: SystemCallHandler::finish_frame() is noexcept but called
//     InvocationFrameStack::log_trace_finish() (-> collect_storage_deltas(),
//     which reserves/appends storage-delta records) with no local catch. It now
//     guards that call and drops the trace on failure.
//   * #2741: finish_frame() restores the parent trace and attaches the finished
//     child trace to the parent's Invoke entry by allocating
//     SyscallInvokeOutputs; that catch used to call panic(). It now reports
//     through the no-allocation reporter and drops the optional child trace
//     instead.
//
// The allocation-failure branches themselves can only be forced with a global
// allocator hook, which (matching
// trace_syscall_logging_2561_2562_regression_test.cc and
// syscall_output_trace_unrecoverable_regression_test.cc) we deliberately do not
// install. These tests pin the success-path invariant: with tracing on, the
// start-syscall / finish-frame / nested-child-attach paths still run, the
// invocation succeeds, and a serialized trace is produced.

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {
namespace {

constexpr ContractAddress kFrameContract{0x42};

void
expect_syscall_ok(ForeignHostFnResult const &result)
{
  EXPECT_EQ(result.panic,
            std::to_underlying(HostFnErrorCode::NoneOrRecoverable));
  EXPECT_EQ(result.result_code,
            std::to_underlying(SyscallErrorCode::kSyscallErrorCodeNone));
}

// Run `body` against a single traced mock invocation frame, then pop the
// context and finish the frame, asserting the finish succeeds and tracing
// produced a serialized trace.
template<typename Body>
void
run_in_traced_frame(Body &&body)
{
  enable_syscall_logging(kTestFFILogLevelTrace);
  x23::Defer reset_logging(
      [] { enable_syscall_logging(kTestFFILogLevelError); });

  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  // Storage-writing syscalls need the frame's contract to hold storage tokens;
  // granting them is harmless for the rest.
  ASSERT_EQ(setup_grant_contract_storage_tokens(db, kFrameContract.data(),
                                                1'000'000'000),
            kTestFFIErrorCodeNone);

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(handler.syscall_handler, nullptr);
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  Invocation invocation = {
      .invoked_address = kFrameContract,
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  body(handler.syscall_handler);

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);
  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_NE(finish_res.serialized_trace.data, nullptr);
  EXPECT_GT(finish_res.serialized_trace.len, 0U);
  free_ffi_data_buffer(finish_res.serialized_output);
  free_ffi_data_buffer(finish_res.serialized_trace);
  free_ffi_data_buffer(finish_res.serialized_events);
}

} // namespace

// #2644: every syscall in a traced frame appends a SyscallTrace record through
// start_syscall_trace() before the noexcept body runs. Running several syscalls
// exercises that append path repeatedly; the frame still finishes and
// serializes its trace.
TEST(TestBindingsTest, TracedFrameAppendsSyscallTracesWithoutCrashing)
{
  run_in_traced_frame([](void * /*handler*/) {
    for (int i = 0; i < 3; ++i) {
      ContractAddress address{};
      auto result = mock_syscall_entrypoint(
          std::to_underlying(SyscallNo::kGetCurrentAddress),
          reinterpret_cast<uint64_t>(&address), 0, 0, 0, 0, 0, 0);
      expect_syscall_ok(result);
      EXPECT_EQ(address, kFrameContract);
    }
  });
}

// #2646: a syscall that writes storage populates the frame's modified items, so
// finishing the traced frame drives log_trace_finish() ->
// collect_storage_deltas(), which reserves/appends storage-delta trace records.
// The guarded finish still produces a serialized trace.
TEST(TestBindingsTest, TracedFrameWithStorageWriteCollectsTraceWithoutCrashing)
{
  run_in_traced_frame([](void * /*handler*/) {
    StorageKeyContractData key{};
    std::array<std::byte, 3> payload{std::byte{0x10}, std::byte{0x20},
                                     std::byte{0x30}};
    auto insert_result = mock_syscall_entrypoint(
        std::to_underlying(SyscallNo::kRetrocausalQueueInsert),
        reinterpret_cast<uint64_t>(&key),
        reinterpret_cast<uint64_t>(payload.data()), payload.size(), 0, 0, 0, 0);
    expect_syscall_ok(insert_result);
  });
}

// #2741: opening a nested child mock frame records a parent Invoke trace entry
// and stashes the parent trace; finishing the child restores the parent and
// attaches the child trace to that Invoke entry (the path that used to
// panic()). Finishing child-then-parent succeeds, serializes traces, and the
// handler closes cleanly.
TEST(TestBindingsTest, TracedNestedChildFrameAttachesChildTraceWithoutCrashing)
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
      .invoked_address = ContractAddress{0xA0},
      .entrypoint = entrypoint_from_method_id(0x1111),
      .gas_limit = 10'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, parent_invocation),
            kTestFFIErrorCodeNone);

  Invocation child_invocation = {
      .invoked_address = ContractAddress{0xB0},
      .entrypoint = entrypoint_from_method_id(0x2222),
      .gas_limit = 5'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, child_invocation),
            kTestFFIErrorCodeNone);

  // Finishing the child restores the parent trace and attaches the child trace
  // to the parent's Invoke entry.
  auto child_finish =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(child_finish.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(child_finish.serialized_output);
  free_ffi_data_buffer(child_finish.serialized_trace);
  free_ffi_data_buffer(child_finish.serialized_events);

  // The parent's serialized trace carries the attached child trace.
  auto parent_finish =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(parent_finish.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_NE(parent_finish.serialized_trace.data, nullptr);
  EXPECT_GT(parent_finish.serialized_trace.len, 0U);
  free_ffi_data_buffer(parent_finish.serialized_output);
  free_ffi_data_buffer(parent_finish.serialized_trace);
  free_ffi_data_buffer(parent_finish.serialized_events);

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

} // namespace hogd
