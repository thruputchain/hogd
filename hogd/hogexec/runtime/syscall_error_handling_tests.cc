#include <array>
#include <cstddef>
#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_meta_concept.h"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class TestRuntime : public Runtime {
public:
  TestRuntime(std::unique_ptr<HostCallContext> host_call_context)
    : Runtime(std::move(host_call_context))
  {}

  ~TestRuntime() override = default;

  [[nodiscard]] uint64_t get_available_gas() const noexcept override
  {
    return available_gas_;
  }
  void set_available_gas(uint64_t gas) noexcept override
  {
    available_gas_ = gas;
  }

  // No memory to access from within a native contract
  std::span<std::byte> get_memory() noexcept override { return {}; }

private:
  InvokeStatus<void> invoke_internal(std::string_view /*unused*/) override
  {
    return {};
  }

  uint64_t available_gas_ = 0;
};

namespace {

template<HostFnErrorCode Code>
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
error_syscall_func() noexcept
{
  return {HostFnError{Code}, 1000};
}
} // namespace

template<HostFnErrorCode Code> struct ErrorSyscallMeta {
  constexpr static bool kIsReadOnlySyscall = false;
  constexpr static auto kInnerFunc = &error_syscall_func<Code>;

  constexpr static SyscallNo kSyscallNo = kNoTracingSyscall;
  constexpr static bool kCanReturn = true;
  constexpr static std::array<std::string_view, 0> kInputNames = {};
  constexpr static std::array<std::string_view, 0> kOutputNames = {};
  constexpr static std::array<SyscallErrorCode, 0> kAllowedErrors = {};
};

static_assert(
    is_syscall_meta<ErrorSyscallMeta<HostFnErrorCode::kHostAbortTransaction>>);
static_assert(
    is_syscall_meta<ErrorSyscallMeta<HostFnErrorCode::kHostOutOfGas>>);
static_assert(
    is_syscall_meta<ErrorSyscallMeta<HostFnErrorCode::kHostUnrecoverable>>);
static_assert(is_syscall_meta<ErrorSyscallMeta<HostFnErrorCode::kHostReturn>>);
static_assert(is_syscall_meta<ErrorSyscallMeta<HostFnErrorCode::kHostExit>>);
static_assert(
    is_syscall_meta<ErrorSyscallMeta<HostFnErrorCode::kHostValidationFailure>>);

// Tests for reference leak bugs in storage item delta management
// These tests verify that when a delta reservation fails, the reference count
// is properly released, allowing subsequent reservations of different types.
class SyscallErrorHandlingTest : public ::testing::Test,
                                 public NativeContractTestFramework {
public:
  SyscallErrorHandlingTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

  constexpr static Invocation kPlaceholderInvocation = {
      .invoked_address = make_contract_address(0),
      .entrypoint = ContractEntrypointID{},
      .gas_limit = 10000,
      .calldata = Calldata{}};

  template<HostFnErrorCode Code> void run_error_override_test()
  {
    // The handler and runtime are entirely irrelevant for this test
    // we just need them to invoke the syscall handler and manipulate available
    // gas
    auto context = make_hogexec_context();
    auto handler = make_new_system_call_handler(Transaction{}, *context);

    auto host_call_context = std::make_unique<HostCallContext>();
    host_call_context->syscall_handler = &handler;

    handler.start_frame(kPlaceholderInvocation,
                        std::to_underlying(InvokeFlag::kInvokeFlagNone));

    // Runtime ctor sets host_call_context->runtime
    auto runtime = TestRuntime(std::move(host_call_context));
    runtime.set_available_gas(10000);
    auto syscall_res = wrap_outer<ErrorSyscallMeta<Code>>(handler, runtime, 0,
                                                          0, 0, 0, 0, 0, 0);
    ASSERT_FALSE(syscall_res.has_value());
    ASSERT_EQ(syscall_res.error().code(), Code);
    ASSERT_EQ(runtime.get_available_gas(), 9000);

    runtime.set_available_gas(10);
    syscall_res = wrap_outer<ErrorSyscallMeta<Code>>(handler, runtime, 0, 0, 0,
                                                     0, 0, 0, 0);
    ASSERT_FALSE(syscall_res.has_value());
    ASSERT_EQ(syscall_res.error().code(), Code);

    auto _ = handler.finish_frame(InvokeStatus<void>{});
  }

  template<HostFnErrorCode Code> void run_gas_limit_supercedes_test()
  {
    // The handler and runtime are entirely irrelevant for this test
    // we just need them to invoke the syscall handler and manipulate available
    // gas
    auto context = make_hogexec_context();
    auto handler = make_new_system_call_handler(Transaction{}, *context);

    auto host_call_context = std::make_unique<HostCallContext>();
    host_call_context->syscall_handler = &handler;

    handler.start_frame(kPlaceholderInvocation,
                        std::to_underlying(InvokeFlag::kInvokeFlagNone));

    // Runtime ctor sets host_call_context->runtime
    auto runtime = TestRuntime(std::move(host_call_context));
    runtime.set_available_gas(10000);
    auto syscall_res = wrap_outer<ErrorSyscallMeta<Code>>(handler, runtime, 0,
                                                          0, 0, 0, 0, 0, 0);
    ASSERT_FALSE(syscall_res.has_value());
    ASSERT_EQ(syscall_res.error().code(), Code);
    ASSERT_EQ(runtime.get_available_gas(), 9000);

    runtime.set_available_gas(10);
    syscall_res = wrap_outer<ErrorSyscallMeta<Code>>(handler, runtime, 0, 0, 0,
                                                     0, 0, 0, 0);
    ASSERT_FALSE(syscall_res.has_value());
    ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::kHostOutOfGas);

    auto _ = handler.finish_frame(InvokeStatus<void>{});
  }
};

TEST_F(SyscallErrorHandlingTest, AbortOverridesGasLimit)
{
  run_error_override_test<HostFnErrorCode::kHostAbortTransaction>();
}

TEST_F(SyscallErrorHandlingTest, UnrecoverableErrorOverridesGasLimit)
{
  run_error_override_test<HostFnErrorCode::kHostUnrecoverable>();
}

TEST_F(SyscallErrorHandlingTest, ValidationFailureOverridesGasLimit)
{
  run_error_override_test<HostFnErrorCode::kHostValidationFailure>();
}

TEST_F(SyscallErrorHandlingTest, GasLimitOverridesReturn)
{
  run_gas_limit_supercedes_test<HostFnErrorCode::kHostReturn>();
}

TEST_F(SyscallErrorHandlingTest, GasLimitOverridesExit)
{
  run_gas_limit_supercedes_test<HostFnErrorCode::kHostExit>();
}

namespace {
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
noreturn_failing_syscall_func() noexcept
{
  return {HostFnError{HostFnErrorCode::kHostUnrecoverable}, 1000};
}
} // namespace

struct NoreturnFailingSyscallMeta {
  constexpr static bool kIsReadOnlySyscall = false;
  constexpr static auto kInnerFunc = &noreturn_failing_syscall_func;

  constexpr static SyscallNo kSyscallNo = kNoTracingSyscall;
  constexpr static bool kCanReturn = false;
  constexpr static std::array<std::string_view, 0> kInputNames = {};
  constexpr static std::array<std::string_view, 0> kOutputNames = {};
  constexpr static std::array<SyscallErrorCode, 0> kAllowedErrors = {};
};

static_assert(is_syscall_meta<NoreturnFailingSyscallMeta>);

TEST_F(SyscallErrorHandlingTest, NoreturnFailingSyscallOverridesGasLimit)
{
  auto context = make_hogexec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  auto host_call_context = std::make_unique<HostCallContext>();
  host_call_context->syscall_handler = &handler;

  handler.start_frame(kPlaceholderInvocation,
                      std::to_underlying(InvokeFlag::kInvokeFlagNone));

  // Runtime ctor sets host_call_context->runtime
  auto runtime = TestRuntime(std::move(host_call_context));
  runtime.set_available_gas(10000);
  auto syscall_res = wrap_outer<NoreturnFailingSyscallMeta>(handler, runtime, 0,
                                                            0, 0, 0, 0, 0, 0);
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::kHostUnrecoverable);
}

namespace {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
noerror_syscall_func() noexcept
{
  return {SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist, 1000};
}
} // namespace

struct NoErrorSyscallMeta {
  constexpr static bool kIsReadOnlySyscall = false;
  constexpr static auto kInnerFunc = &noerror_syscall_func;

  constexpr static SyscallNo kSyscallNo = kNoTracingSyscall;
  constexpr static bool kCanReturn = true;
  constexpr static std::array<std::string_view, 0> kInputNames = {};
  constexpr static std::array<std::string_view, 0> kOutputNames = {};
  constexpr static std::array<SyscallErrorCode, 1> kAllowedErrors = {
      SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist};
};

static_assert(is_syscall_meta<NoErrorSyscallMeta>);

TEST_F(SyscallErrorHandlingTest, NoerrorOutOfGas)
{
  auto context = make_hogexec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  auto host_call_context = std::make_unique<HostCallContext>();
  host_call_context->syscall_handler = &handler;

  handler.start_frame(kPlaceholderInvocation,
                      std::to_underlying(InvokeFlag::kInvokeFlagNone));

  // Runtime ctor sets host_call_context->runtime
  auto runtime = TestRuntime(std::move(host_call_context));
  runtime.set_available_gas(10000);
  auto syscall_res =
      wrap_outer<NoErrorSyscallMeta>(handler, runtime, 0, 0, 0, 0, 0, 0, 0);
  ASSERT_TRUE(syscall_res.has_value());

  runtime.set_available_gas(10);
  syscall_res =
      wrap_outer<NoErrorSyscallMeta>(handler, runtime, 0, 0, 0, 0, 0, 0, 0);
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::kHostOutOfGas);
}

struct NoReturnNoErrorSyscallMeta {
  constexpr static bool kIsReadOnlySyscall = false;
  constexpr static auto kInnerFunc = &noerror_syscall_func;

  constexpr static SyscallNo kSyscallNo = static_cast<SyscallNo>(-1);
  constexpr static bool kCanReturn = false;
  constexpr static std::array<std::string_view, 0> kInputNames = {};
  constexpr static std::array<std::string_view, 0> kOutputNames = {};
  constexpr static std::array<SyscallErrorCode, 1> kAllowedErrors = {
      SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist};
};

static_assert(is_syscall_meta<NoReturnNoErrorSyscallMeta>);

TEST_F(SyscallErrorHandlingTest, NoreturnNoErrorOutOfGas)
{
  auto context = make_hogexec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  auto host_call_context = std::make_unique<HostCallContext>();
  host_call_context->syscall_handler = &handler;

  handler.start_frame(kPlaceholderInvocation,
                      std::to_underlying(InvokeFlag::kInvokeFlagNone));

  // Runtime ctor sets host_call_context->runtime
  auto runtime = TestRuntime(std::move(host_call_context));
  runtime.set_available_gas(10000);
  auto syscall_res = wrap_outer<NoReturnNoErrorSyscallMeta>(handler, runtime, 0,
                                                            0, 0, 0, 0, 0, 0);
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::kHostExit);

  runtime.set_available_gas(10);
  syscall_res = wrap_outer<NoReturnNoErrorSyscallMeta>(handler, runtime, 0, 0,
                                                       0, 0, 0, 0, 0);
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::kHostOutOfGas);
}

struct NoReturnReadOnlySyscallMeta {
  constexpr static bool kIsReadOnlySyscall = false;
  constexpr static auto kInnerFunc = &noerror_syscall_func;

  constexpr static SyscallNo kSyscallNo = static_cast<SyscallNo>(-1);
  constexpr static bool kCanReturn = false;
  constexpr static std::array<std::string_view, 0> kInputNames = {};
  constexpr static std::array<std::string_view, 0> kOutputNames = {};
  constexpr static std::array<SyscallErrorCode, 2> kAllowedErrors = {
      SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist,
      SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
};

static_assert(is_syscall_meta<NoReturnReadOnlySyscallMeta>);

TEST_F(SyscallErrorHandlingTest, NoReturnReadOnly)
{
  auto context = make_hogexec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  auto host_call_context = std::make_unique<HostCallContext>();
  host_call_context->syscall_handler = &handler;

  handler.start_frame(kPlaceholderInvocation,
                      std::to_underlying(InvokeFlag::kInvokeFlagReadOnly));

  // Runtime ctor sets host_call_context->runtime
  auto runtime = TestRuntime(std::move(host_call_context));
  runtime.set_available_gas(10000);
  auto syscall_res =
      wrap_outer<NoReturnReadOnlySyscallMeta>(handler, runtime, 0, 0, 0, 0, 0,
                                              0, 0);
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::kHostExit);
  // Gas is not consumed by the syscall.
  // TODO(geoff): This will break when we add a default gas cost for the error
  // handling (calling a non-readonly syscal in read-only mode should cost at
  // least some gas)
  ASSERT_EQ(runtime.get_available_gas(), 10000);

  runtime.set_available_gas(10);
  syscall_res = wrap_outer<NoReturnReadOnlySyscallMeta>(handler, runtime, 0, 0,
                                                        0, 0, 0, 0, 0);
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::kHostExit);
  // TODO(geoff): same as above
  ASSERT_EQ(runtime.get_available_gas(), 10);
}

} // namespace hogd
