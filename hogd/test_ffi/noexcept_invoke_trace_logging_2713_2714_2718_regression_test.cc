// Regression test for issues #2713, #2714, and #2718.
//
// These three diagnostics all run on the child-Invoke path, which is reached
// from the noexcept syscall_Invoke_inner()() body:
//   #2713 - Runtime::invoke() emits an unguarded entry trace
//           ("Invoking method {} with gas limit {}").
//   #2718 - Runtime::invoke() emits an unguarded result trace
//           ("Invoke result: {}, gas consumed: {}").
//   #2714 - SystemCallHandler::invoke() emits an unguarded trace
//           ("SystemError: resolving runtime for address: {}, error {}") when
//           the target address resolves to a null runtime.
// In every case the logger formats into heap-backed buffers, so an allocation
// (or sink) failure throws straight across the noexcept boundary and calls
// std::terminate() before the child status/gas can be returned.
//
// Each throw is now caught and treated as out-of-memory: the invocation fails
// as Unrecoverable rather than crossing the noexcept boundary. We force the
// throw deterministically with a logger keyed to the specific diagnostic
// (rather than a global allocator hook) and assert root_invoke() returns a
// structured Unrecoverable result instead of terminating the process.

#include <atomic>
#include <cstdint>
#include <format>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/serialize.h"

namespace hogd {
namespace {

// Set true once the root mock contract is about to issue (or has issued) the
// child Invoke, so the throwing loggers below only fire on the child's
// diagnostics and not on the root's identical diagnostics.
std::atomic<bool> g_in_child_invoke = false;

// The #2713/#2718 child-Invoke diagnostics ("Invoking method ..." and "Invoke
// result: ...") are emitted at kTrace, which is compiled out in release builds
// (kMaxLogLevel == kInfo). When the trace site does not exist, the throwing
// logger is never invoked, so the child Invoke completes normally and the
// result is None instead of Unrecoverable. The #2714 null-runtime test is
// unaffected: an undeployed child fails Unrecoverable regardless of the trace.
constexpr enum TestFFIErrorCode kExpectedChildInvokeErrorCode =
    should_log(LogLevel::kTrace) ? kTestFFIErrorCodeUnrecoverableError
                                 : kTestFFIErrorCodeNone;

// Issues a child Invoke syscall (mirrors test_bindings_test.cc's local helper).
HostFnStatus<SyscallErrorCode>
issue_child_invoke(const ContractAddress &address)
{
  CopyableInvokeInputs inputs{
      .entrypoint_id = 0x1234,
      .gas_limit = UINT64_MAX,
      .flags = InvokeFlagSet{},
  };
  CopyableInvokeOutputs outputs{};
  Calldata empty_calldata;

  auto result =
      mock_syscall_entrypoint(std::to_underlying(SyscallNo::kInvoke),
                              reinterpret_cast<uint64_t>(address.data()),
                              reinterpret_cast<uint64_t>(empty_calldata.data()),
                              empty_calldata.size(), 0, 0,
                              reinterpret_cast<uint64_t>(&inputs),
                              reinterpret_cast<uint64_t>(&outputs));

  if (result.panic != std::to_underlying(HostFnErrorCode::NoneOrRecoverable))
    return HostFnError{static_cast<HostFnErrorCode>(result.panic)};
  return static_cast<SyscallErrorCode>(result.result_code);
}

ForeignInvokeResult
child_success(void * /*unused*/, uint64_t /*method_number*/)
{
  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

// Root mock contract: invokes the child address handed to it via the foreign
// context, then returns success/failure based on the child result.
ForeignInvokeResult
root_invokes_child(void *foreign_context, uint64_t /*method_number*/)
{
  auto *child_addr = static_cast<ContractAddress *>(foreign_context);
  g_in_child_invoke.store(true, std::memory_order_relaxed);
  auto res = issue_child_invoke(*child_addr);
  g_in_child_invoke.store(false, std::memory_order_relaxed);

  if (!res.has_value() || *res != SyscallErrorCode::kSyscallErrorCodeNone) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }
  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

// Deploy a root mock contract that invokes child_addr, optionally deploy a
// child mock contract, and return a real (non-mocked) syscall handler ready for
// root_invoke().
struct InvokeHarness {
  void *db = nullptr;
  void *rc = nullptr;
  void *syscall_handler = nullptr;
  // The logger displaced by run_with_logger(), restored in teardown() after
  // free_db() so the global logger pointer is never swapped while the DB
  // background worker thread is alive.
  std::unique_ptr<Logger> previous_logger = nullptr;
};

InvokeHarness
setup_root_invokes_child(ContractAddress *child_addr_storage, bool deploy_child)
{
  InvokeHarness h;
  h.db = make_db();
  EXPECT_NE(h.db, nullptr);
  h.rc = make_runtime_context();
  EXPECT_NE(h.rc, nullptr);

  ContractAddress root_addr{};
  *child_addr_storage = ContractAddress{};
  (*child_addr_storage)[0] = 1;

  EXPECT_EQ(register_new_contract(h.rc, 0, root_invokes_child,
                                  child_addr_storage),
            kTestFFIErrorCodeNone);
  Contract root_contract(ContractType::kMockTest);
  root_contract.mock_contract_id() = 0;
  auto root_bytes = x23::serialize(root_contract);
  EXPECT_EQ(setup_create_contract(h.rc, h.db, root_bytes.data(),
                                  root_bytes.size()),
            kTestFFIErrorCodeNone);
  EXPECT_EQ(setup_deploy_contract(h.db, root_addr.data(),
                                  sha256(root_contract).data(),
                                  root_bytes.size()),
            kTestFFIErrorCodeNone);
  EXPECT_EQ(setup_grant_contract_storage_tokens(h.db, root_addr.data(),
                                                1'000'000'000),
            kTestFFIErrorCodeNone);

  if (deploy_child) {
    EXPECT_EQ(register_new_contract(h.rc, 1, child_success, nullptr),
              kTestFFIErrorCodeNone);
    Contract child_contract(ContractType::kMockTest);
    child_contract.mock_contract_id() = 1;
    auto child_bytes = x23::serialize(child_contract);
    EXPECT_EQ(setup_create_contract(h.rc, h.db, child_bytes.data(),
                                    child_bytes.size()),
              kTestFFIErrorCodeNone);
    EXPECT_EQ(setup_deploy_contract(h.db, child_addr_storage->data(),
                                    sha256(child_contract).data(),
                                    child_bytes.size()),
              kTestFFIErrorCodeNone);
    EXPECT_EQ(setup_grant_contract_storage_tokens(h.db,
                                                  child_addr_storage->data(),
                                                  1'000'000'000),
              kTestFFIErrorCodeNone);
  }

  EXPECT_EQ(advance_new_block(h.db), kTestFFIErrorCodeNone);

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = root_addr,
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
free_result_buffers(ForeignTracedInvokeResult &result)
{
  free_ffi_data_buffer(result.serialized_output);
  free_ffi_data_buffer(result.serialized_trace);
  free_ffi_data_buffer(result.serialized_events);
}

// Throws on a single diagnostic fmt, gated so only the child's diagnostic
// fires (not the root's identical one).
class ThrowingDiagnosticLogger : public Logger {
public:
  ThrowingDiagnosticLogger(std::string_view target_fmt, bool gate_on_child)
    : target_fmt_(target_fmt), gate_on_child_(gate_on_child)
  {}

  void vlog(LogLevel level, std::string_view fmt,
            const std::source_location & /*loc*/,
            std::format_args /*args*/) override
  {
    if (level == LogLevel::kTrace && fmt == target_fmt_ &&
        (!gate_on_child_ ||
         g_in_child_invoke.load(std::memory_order_relaxed))) {
      throw std::runtime_error("throwing diagnostic logger");
    }
  }

  void flush() override {}
  LogLevel get_max_log_level() const override { return LogLevel::kTrace; }

private:
  std::string_view target_fmt_;
  bool gate_on_child_;
};

InvokeHarness
run_with_logger(ContractAddress *child_addr_storage, bool deploy_child,
                std::string_view target_fmt, bool gate_on_child,
                ForeignTracedInvokeResult &result_out)
{
  g_in_child_invoke.store(false, std::memory_order_relaxed);
  enable_syscall_logging(kTestFFILogLevelTrace);

  // Install the throwing logger before make_db() (called inside setup) and
  // restore it after free_db() (in teardown). The DB spawns a background worker
  // thread that logs through the global logger; swapping that global pointer
  // while the worker is alive is a data race (TSAN flags hogd::swap_logger vs
  // the worker's read). The diagnostic logger is gated on the targeted trace,
  // so the DB's own logging and the teardown path are unaffected.
  auto previous_logger = swap_logger(
      std::make_unique<ThrowingDiagnosticLogger>(target_fmt, gate_on_child));

  auto harness = setup_root_invokes_child(child_addr_storage, deploy_child);
  harness.previous_logger = std::move(previous_logger);

  result_out = root_invoke(harness.syscall_handler, /*enable_tracing=*/1);
  enable_syscall_logging(kTestFFILogLevelError);
  return harness;
}

void
teardown(InvokeHarness &h)
{
  close_syscall_handler(h.syscall_handler);
  close_runtime_context(h.rc);
  free_db(h.db);
  // Restore the logger only after free_db() has joined the DB background worker
  // thread, so the global logger pointer is not swapped out from under it.
  swap_logger(std::move(h.previous_logger));
}

} // namespace

// The child Runtime::invoke() entry trace throwing must not terminate the
// process; the logging failure is treated as out-of-memory and surfaced as an
// Unrecoverable child invocation, which the root propagates to root_invoke()
// (issue #2713).
TEST(TestBindingsTest, ChildInvokeEntryTraceLoggerThrowDoesNotTerminate)
{
  ContractAddress child_addr{};
  ForeignTracedInvokeResult result{};
  auto h = run_with_logger(&child_addr, /*deploy_child=*/true,
                           "Invoking method {} with gas limit {}",
                           /*gate_on_child=*/true, result);
  x23::Defer d([&] { teardown(h); });
  x23::Defer fb([&] { free_result_buffers(result); });

  EXPECT_EQ(result.ffi_error_code, kExpectedChildInvokeErrorCode);
}

// The child Runtime::invoke() result trace throwing must not terminate the
// process after the child has already produced its result; it is surfaced as an
// Unrecoverable invocation instead (issue #2718).
TEST(TestBindingsTest, ChildInvokeResultTraceLoggerThrowDoesNotTerminate)
{
  ContractAddress child_addr{};
  ForeignTracedInvokeResult result{};
  auto h = run_with_logger(&child_addr, /*deploy_child=*/true,
                           "Invoke result: {}, gas consumed: {}",
                           /*gate_on_child=*/true, result);
  x23::Defer d([&] { teardown(h); });
  x23::Defer fb([&] { free_result_buffers(result); });

  EXPECT_EQ(result.ffi_error_code, kExpectedChildInvokeErrorCode);
}

// SystemCallHandler::invoke()'s null-runtime trace throwing must not terminate
// the process; the child Invoke fails as Unrecoverable instead (issue #2714).
// The child address is never deployed, so resolve_runtime() returns a null
// runtime and the diagnostic fires.
TEST(TestBindingsTest, NullRuntimeTraceLoggerThrowDoesNotTerminate)
{
  ContractAddress child_addr{};
  ForeignTracedInvokeResult result{};
  auto h = run_with_logger(&child_addr, /*deploy_child=*/false,
                           "Failed to instantiate runtime for address: {}",
                           /*gate_on_child=*/false, result);
  x23::Defer d([&] { teardown(h); });
  x23::Defer fb([&] { free_result_buffers(result); });

  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);
}

} // namespace hogd
