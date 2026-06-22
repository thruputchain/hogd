// Regression test for issue #2717.
//
// syscall_ConsoleLog_inner() runs inside a noexcept syscall body and emits an
// unguarded trace("ConsoleLog: {}", ...). The active logger formats that
// diagnostic into heap-backed buffers, so an allocation (or sink) failure can
// throw straight across the noexcept boundary and call std::terminate() before
// the mock entrypoint's catch can translate it into a structured FFI result.
//
// The throw is now caught and the logging failure is treated as out-of-memory:
// the syscall returns a structured Unrecoverable host-function error instead of
// terminating. We force the throw deterministically with a logger that only
// throws on the ConsoleLog inner trace (rather than a global allocator hook),
// and assert the call returns Unrecoverable instead of terminating the process.

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {
namespace {

// Throws only when syscall_ConsoleLog_inner() emits its own trace line, leaving
// every other diagnostic (e.g. the wrapper's "Syscall {} Input/Output" traces)
// untouched.
class ThrowingConsoleLogLogger : public Logger {
public:
  void vlog(LogLevel level, std::string_view fmt,
            const std::source_location &loc, std::format_args /*args*/) override
  {
    if (level == LogLevel::kTrace && fmt == "ConsoleLog: {}" &&
        std::string_view(loc.file_name()).ends_with("syscall_log.cc")) {
      throw std::runtime_error("throwing ConsoleLog trace logger");
    }
  }

  void flush() override {}

  LogLevel get_max_log_level() const override { return LogLevel::kTrace; }
};

ForeignHostFnResult
call_console_log(std::span<std::byte> input_buffer)
{
  return mock_syscall_entrypoint(std::to_underlying(SyscallNo::kConsoleLog),
                                 reinterpret_cast<uint64_t>(
                                     input_buffer.data()),
                                 input_buffer.size(), 0, 0, 0, 0, 0);
}

} // namespace

// A traced ConsoleLog syscall whose inner trace logger throws must not
// terminate the process: the failure is treated as out-of-memory and the
// syscall returns a structured Unrecoverable error (issue #2717).
TEST(TestBindingsTest, ConsoleLogInnerTraceLoggerThrowDoesNotTerminate)
{
  enable_syscall_logging(kTestFFILogLevelTrace);
  x23::Defer reset_logging(
      [] { enable_syscall_logging(kTestFFILogLevelError); });

  // Install the throwing logger before make_db() and restore it after free_db()
  // (Defers run in reverse order, so reset_logger runs after d). The DB spawns
  // a background worker thread that logs through the global logger; swapping
  // that global pointer while the worker is alive is a data race (TSAN flags
  // hogd::swap_logger vs the worker's read). Bracketing the DB lifetime keeps
  // the pointer constant for the worker's entire lifetime. The throwing logger
  // only throws on the ConsoleLog inner trace, so the DB's own logging and the
  // teardown path are unaffected.
  auto previous_logger =
      swap_logger(std::make_unique<ThrowingConsoleLogLogger>());
  x23::Defer reset_logger([&] { swap_logger(std::move(previous_logger)); });

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

  Invocation invocation = {
      .invoked_address = ContractAddress{0x42},
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  std::array<std::byte, 4> input{};
  auto result = call_console_log(input);

  // The throwing inner trace must not terminate the process. Instead the
  // syscall surfaces the logging failure (treated as out-of-memory) as a
  // structured Unrecoverable host-function error.
  EXPECT_EQ(result.panic, std::to_underlying(HostFnErrorCode::Unrecoverable));

  // Finish the mock frame so the handler closes cleanly during teardown; an
  // unfinished frame leaves the handler open and free_db() hangs on exit. The
  // throwing logger only throws on the ConsoleLog inner trace, so it does not
  // interfere with the finish path or any teardown logging (the normal logger
  // is restored last, after free_db()).
  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);
  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(finish_res.serialized_output);
  free_ffi_data_buffer(finish_res.serialized_trace);
  free_ffi_data_buffer(finish_res.serialized_events);
}

} // namespace hogd
