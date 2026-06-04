// Regression test for issues #2561 and #2562. See test_ffi_test_helpers.h.
//
// wrap_after_parse() runs the syscall body inside a noexcept lambda so gas
// accounting can never be skipped. Two best-effort diagnostic steps inside that
// lambda used to be able to terminate an honest test_ffi caller:
//   #2561 - copying the syscall inputs into the XDR trace record caught
//           allocation failure and called panic(...).
//   #2562 - formatting the syscall input/output for the trace log allocated and
//           could throw straight across the noexcept boundary.
// Both are now best-effort (drop the record/log line and keep the real syscall
// result). This test exercises those paths under trace-level logging with
// tracing enabled and asserts the syscall still succeeds and a trace is
// produced. The allocation-failure crash itself can only be forced with a
// global allocator hook, which we deliberately do not install here.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
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

ForeignHostFnResult
call_get_calldata(std::span<std::byte> output_buffer,
                  RuntimeMemoryLength &output_written_len,
                  std::span<std::byte> output_auth_token_list,
                  RuntimeMemoryLength &output_auth_token_written_len)
{
  return mock_syscall_entrypoint(
      std::to_underlying(SyscallNo::kGetCalldata),
      reinterpret_cast<uint64_t>(output_buffer.data()), output_buffer.size(),
      reinterpret_cast<uint64_t>(&output_written_len),
      reinterpret_cast<uint64_t>(output_auth_token_list.data()),
      output_auth_token_list.size(),
      reinterpret_cast<uint64_t>(&output_auth_token_written_len), 0);
}

} // namespace

// With trace logging enabled and a traced mocked handler, a real public mock
// syscall must run its trace-input copy and trace input/output formatting
// (issues #2561, #2562) without terminating, return success, and still produce
// a serialized trace.
TEST(TestBindingsTest, TracedMockSyscallRunsTraceDiagnosticsWithoutCrashing)
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

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(handler.syscall_handler, nullptr);
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  Calldata calldata = {0x42, 0x11, 0x22, 0x33};
  Invocation invocation = {
      .invoked_address = ContractAddress{0x42},
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = calldata,
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  std::array<std::byte, 4096> output{};
  RuntimeMemoryLength written_len = 0;
  std::array<std::byte, 4> auth_token_output{};
  RuntimeMemoryLength auth_token_written_len = 0;
  auto result = call_get_calldata(output, written_len, auth_token_output,
                                  auth_token_written_len);

  // The syscall ran to completion through the trace-diagnostic paths.
  EXPECT_EQ(result.panic,
            std::to_underlying(HostFnErrorCode::NoneOrRecoverable));
  EXPECT_EQ(result.result_code,
            std::to_underlying(SyscallErrorCode::kSyscallErrorCodeNone));
  EXPECT_EQ(written_len, calldata.size());
  EXPECT_EQ(output[0], std::byte{0x42});
  EXPECT_EQ(output[1], std::byte{0x11});
  EXPECT_EQ(auth_token_written_len, 0U);

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);
  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  // Tracing was enabled, so the finished frame carries a serialized trace that
  // includes the GetCalldata syscall record built by the now-best-effort paths.
  EXPECT_NE(finish_res.serialized_trace.data, nullptr);
  EXPECT_GT(finish_res.serialized_trace.len, 0U);
  free_ffi_data_buffer(finish_res.serialized_output);
  free_ffi_data_buffer(finish_res.serialized_trace);
  free_ffi_data_buffer(finish_res.serialized_events);
}

} // namespace hogd
