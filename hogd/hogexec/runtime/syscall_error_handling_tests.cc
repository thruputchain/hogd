#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "hogd/hogexec/runtime/syscall_meta_concept.h"
#include "hogd/hogexec/runtime/syscall_types.h"
#include "invoke_flag.hh"
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

private:
  InvokeStatus<void> invoke_internal(std::string_view /*unused*/) override
  {
    return {};
  }

  uint64_t available_gas_ = 0;
};

namespace {

template<HostFnErrorCode Code>
std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
error_syscall_func() noexcept
{
  return {HostFnError{Code}, WrappedGasAmount{1000}};
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

static_assert(is_syscall_meta<ErrorSyscallMeta<HostFnErrorCode::Abandon>>);
static_assert(is_syscall_meta<ErrorSyscallMeta<HostFnErrorCode::OutOfGas>>);
static_assert(
    is_syscall_meta<ErrorSyscallMeta<HostFnErrorCode::Unrecoverable>>);
static_assert(is_syscall_meta<ErrorSyscallMeta<HostFnErrorCode::Return>>);
static_assert(is_syscall_meta<ErrorSyscallMeta<HostFnErrorCode::Exit>>);
static_assert(
    is_syscall_meta<ErrorSyscallMeta<HostFnErrorCode::ValidationFailure>>);

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
                        std::to_underlying(InvokeFlag::None),
                        std::span<uint32_t>{});

    // Runtime ctor sets host_call_context->runtime
    auto runtime = TestRuntime(std::move(host_call_context));
    runtime.set_available_gas(10000);
    auto syscall_res =
        wrap_outer<ErrorSyscallMeta<Code>>(handler, runtime,
                                           LinearMemorySlice{},
                                           kNoTracingSyscall, 0, 0, 0, 0, 0, 0,
                                           0, 0, 0, 0);
    ASSERT_FALSE(syscall_res.has_value());
    ASSERT_EQ(syscall_res.error().code(), Code);
    ASSERT_EQ(runtime.get_available_gas(), 9000);

    runtime.set_available_gas(10);
    syscall_res = wrap_outer<ErrorSyscallMeta<Code>>(handler, runtime,
                                                     LinearMemorySlice{},
                                                     kNoTracingSyscall, 0, 0, 0,
                                                     0, 0, 0, 0, 0, 0, 0);
    ASSERT_FALSE(syscall_res.has_value());
    ASSERT_EQ(syscall_res.error().code(), Code);

    auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr);
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
                        std::to_underlying(InvokeFlag::None),
                        std::span<uint32_t>{});

    // Runtime ctor sets host_call_context->runtime
    auto runtime = TestRuntime(std::move(host_call_context));
    runtime.set_available_gas(10000);
    auto syscall_res =
        wrap_outer<ErrorSyscallMeta<Code>>(handler, runtime,
                                           LinearMemorySlice{},
                                           kNoTracingSyscall, 0, 0, 0, 0, 0, 0,
                                           0, 0, 0, 0);
    ASSERT_FALSE(syscall_res.has_value());
    ASSERT_EQ(syscall_res.error().code(), Code);
    ASSERT_EQ(runtime.get_available_gas(), 9000);

    runtime.set_available_gas(10);
    syscall_res = wrap_outer<ErrorSyscallMeta<Code>>(handler, runtime,
                                                     LinearMemorySlice{},
                                                     kNoTracingSyscall, 0, 0, 0,
                                                     0, 0, 0, 0, 0, 0, 0);
    ASSERT_FALSE(syscall_res.has_value());
    ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::OutOfGas);

    auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr);
  }
};

TEST_F(SyscallErrorHandlingTest, AbortOverridesGasLimit)
{
  run_error_override_test<HostFnErrorCode::Abandon>();
}

TEST_F(SyscallErrorHandlingTest, UnrecoverableErrorOverridesGasLimit)
{
  run_error_override_test<HostFnErrorCode::Unrecoverable>();
}

TEST_F(SyscallErrorHandlingTest, ValidationFailureOverridesGasLimit)
{
  run_error_override_test<HostFnErrorCode::ValidationFailure>();
}

TEST_F(SyscallErrorHandlingTest, GasLimitOverridesReturn)
{
  run_gas_limit_supercedes_test<HostFnErrorCode::Return>();
}

TEST_F(SyscallErrorHandlingTest, GasLimitOverridesExit)
{
  run_gas_limit_supercedes_test<HostFnErrorCode::Exit>();
}

namespace {
std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
noreturn_failing_syscall_func() noexcept
{
  return {HostFnError{HostFnErrorCode::Unrecoverable}, WrappedGasAmount{1000}};
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
                      std::to_underlying(InvokeFlag::None),
                      std::span<uint32_t>{});

  // Runtime ctor sets host_call_context->runtime
  auto runtime = TestRuntime(std::move(host_call_context));
  runtime.set_available_gas(10000);
  auto syscall_res =
      wrap_outer<NoreturnFailingSyscallMeta>(handler, runtime,
                                             LinearMemorySlice{},
                                             kNoTracingSyscall, 0, 0, 0, 0, 0,
                                             0, 0, 0, 0, 0);
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::Unrecoverable);
}

namespace {

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
noerror_syscall_func() noexcept
{
  return {SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist,
          WrappedGasAmount{1000}};
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
                      std::to_underlying(InvokeFlag::None),
                      std::span<uint32_t>{});

  // Runtime ctor sets host_call_context->runtime
  auto runtime = TestRuntime(std::move(host_call_context));
  runtime.set_available_gas(10000);
  auto syscall_res =
      wrap_outer<NoErrorSyscallMeta>(handler, runtime, LinearMemorySlice{},
                                     kNoTracingSyscall, 0, 0, 0, 0, 0, 0, 0, 0,
                                     0, 0);
  ASSERT_TRUE(syscall_res.has_value());

  runtime.set_available_gas(10);
  syscall_res =
      wrap_outer<NoErrorSyscallMeta>(handler, runtime, LinearMemorySlice{},
                                     kNoTracingSyscall, 0, 0, 0, 0, 0, 0, 0, 0,
                                     0, 0);
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::OutOfGas);
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
                      std::to_underlying(InvokeFlag::None),
                      std::span<uint32_t>{});

  // Runtime ctor sets host_call_context->runtime
  auto runtime = TestRuntime(std::move(host_call_context));
  runtime.set_available_gas(10000);
  auto syscall_res =
      wrap_outer<NoReturnNoErrorSyscallMeta>(handler, runtime,
                                             LinearMemorySlice{},
                                             kNoTracingSyscall, 0, 0, 0, 0, 0,
                                             0, 0, 0, 0, 0);
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::Exit);

  runtime.set_available_gas(10);
  syscall_res = wrap_outer<NoReturnNoErrorSyscallMeta>(handler, runtime,
                                                       LinearMemorySlice{},
                                                       kNoTracingSyscall, 0, 0,
                                                       0, 0, 0, 0, 0, 0, 0, 0);
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::OutOfGas);
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
                      std::to_underlying(InvokeFlag::ReadOnly),
                      std::span<uint32_t>{});

  // Runtime ctor sets host_call_context->runtime
  auto runtime = TestRuntime(std::move(host_call_context));
  runtime.set_available_gas(10000);
  auto syscall_res =
      wrap_outer<NoReturnReadOnlySyscallMeta>(handler, runtime,
                                              LinearMemorySlice{},
                                              kNoTracingSyscall, 0, 0, 0, 0, 0,
                                              0, 0, 0, 0, 0);
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::Exit);
  // Gas is not consumed by the syscall.
  // TODO(geoff): This will break when we add a default gas cost for the error
  // handling (calling a non-readonly syscal in read-only mode should cost at
  // least some gas)
  ASSERT_EQ(runtime.get_available_gas(), 10000);

  runtime.set_available_gas(10);
  syscall_res = wrap_outer<NoReturnReadOnlySyscallMeta>(handler, runtime,
                                                        LinearMemorySlice{},
                                                        kNoTracingSyscall, 0, 0,
                                                        0, 0, 0, 0, 0, 0, 0, 0);
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::Exit);
  // TODO(geoff): same as above
  ASSERT_EQ(runtime.get_available_gas(), 10);
}

// WrappedGasAmount must not be implicitly constructible from int,
// ensuring that accidentally returning a non-gas value as the gas amount
// is a compile error.
static_assert(!std::is_convertible_v<int, WrappedGasAmount>);
static_assert(!std::is_convertible_v<int32_t, WrappedGasAmount>);
static_assert(!std::is_convertible_v<uint64_t, WrappedGasAmount>);
static_assert(!std::is_convertible_v<GasAmount, WrappedGasAmount>);

// Regression test for the bug in syscall_Return_inner where
// the gas amount field was accidentally set to a cast of
// SyscallErrorCode::kSyscallErrorCodeDeterministicWasm instead of 0.
TEST_F(SyscallErrorHandlingTest, ReturnOversizedBufferGasIsZero)
{
  auto context = make_hogexec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  auto host_call_context = std::make_unique<HostCallContext>();
  host_call_context->syscall_handler = &handler;

  handler.start_frame(kPlaceholderInvocation,
                      std::to_underlying(InvokeFlag::None),
                      std::span<uint32_t>{});

  // Create a buffer larger than kMaxReturnBufferLen (1024)
  std::array<std::byte, 1025> oversized_buffer{};

  auto [status, gas] =
      syscall_Return_inner(handler,
                           std::span<const std::byte>(oversized_buffer), false,
                           {});

  // The syscall should fail with Exit
  ASSERT_FALSE(status.has_value());
  ASSERT_EQ(status.error().code(), HostFnErrorCode::Exit);

  // The gas consumed must be 0, not an error code value
  ASSERT_EQ(gas.get(), 0);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr);
}

// Test that syscall_Return_inner with a valid buffer returns 0 gas consumed
TEST_F(SyscallErrorHandlingTest, ReturnValidBufferGasIsZero)
{
  auto context = make_hogexec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  auto host_call_context = std::make_unique<HostCallContext>();
  host_call_context->syscall_handler = &handler;

  handler.start_frame(kPlaceholderInvocation,
                      std::to_underlying(InvokeFlag::None),
                      std::span<uint32_t>{});

  std::array<std::byte, 10> valid_buffer{};

  auto [status, gas] =
      syscall_Return_inner(handler, std::span<const std::byte>(valid_buffer),
                           false, {});

  // The syscall should succeed with Return
  ASSERT_FALSE(status.has_value());
  ASSERT_EQ(status.error().code(), HostFnErrorCode::Return);

  // The gas consumed must be 0
  ASSERT_EQ(gas.get(), 0);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr);
}

} // namespace hogd
