#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "auth_claim.hh"
#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/syscall_entry_outer.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "hogd/hog_exec/runtime/syscall_meta_concept.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

namespace {

template<HostFnErrorCode Code>
std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
error_syscall_func() noexcept
{
  return {HostFnError{Code}, WrappedGasAmount{1000}};
}
} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
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

// NOLINTNEXTLINE(misc-use-internal-linkage)
class SyscallErrorHandlingTest : public ::testing::Test,
                                 public NativeContractTestFramework {
public:
  SyscallErrorHandlingTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

  constexpr static Invocation kPlaceholderInvocation = {
      .invoked_address = make_contract_address(0),
      .entrypoint = ContractEntrypointID{},
      .gas_limit = 10000,
      .calldata = Calldata{}};

  constexpr static WasmContext make_null_context(GasAmount gas_limit)
  {
    return WasmContext{
        .memory = LinearMemorySlice{},
        .gas_remaining = gas_limit,
    };
  }

  template<HostFnErrorCode Code> void run_error_override_test()
  {
    // The handler and runtime are entirely irrelevant for this test
    // we just need them to invoke the syscall handler and manipulate available
    // gas
    auto context = make_hog_exec_context();
    auto handler = make_new_system_call_handler(Transaction{}, *context);

    handler.start_frame(kPlaceholderInvocation,
                        std::to_underlying(InvokeFlag::None),
                        std::span<uint32_t>{});

    auto [syscall_res, gas_consumed] =
        wrap_outer<ErrorSyscallMeta<Code>>(handler, make_null_context(10000),
                                           kNoTracingSyscall, 0, 0, 0, 0, 0, 0,
                                           0);
    ASSERT_FALSE(syscall_res.has_value());
    ASSERT_EQ(syscall_res.error().code(), Code);

    auto [syscall_res2, gas_consumed2] =
        wrap_outer<ErrorSyscallMeta<Code>>(handler, make_null_context(10),
                                           kNoTracingSyscall, 0, 0, 0, 0, 0, 0,
                                           0);
    ASSERT_FALSE(syscall_res2.has_value());
    ASSERT_EQ(syscall_res2.error().code(), Code);
    ASSERT_EQ(gas_consumed2, 10);

    auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr);
  }

  template<HostFnErrorCode Code> void run_gas_limit_supercedes_test()
  {
    // The handler and runtime are entirely irrelevant for this test
    // we just need them to invoke the syscall handler and manipulate available
    // gas
    auto context = make_hog_exec_context();
    auto handler = make_new_system_call_handler(Transaction{}, *context);

    handler.start_frame(kPlaceholderInvocation,
                        std::to_underlying(InvokeFlag::None),
                        std::span<uint32_t>{});

    auto [syscall_res, gas_consumed] =
        wrap_outer<ErrorSyscallMeta<Code>>(handler, make_null_context(10000),
                                           kNoTracingSyscall, 0, 0, 0, 0, 0, 0,
                                           0);
    ASSERT_FALSE(syscall_res.has_value());
    ASSERT_EQ(syscall_res.error().code(), Code);
    ASSERT_EQ(gas_consumed, 1000);

    auto [syscall_res2, gas_consumed2] =
        wrap_outer<ErrorSyscallMeta<Code>>(handler, make_null_context(10),
                                           kNoTracingSyscall, 0, 0, 0, 0, 0, 0,
                                           0);
    ASSERT_FALSE(syscall_res2.has_value());
    ASSERT_EQ(syscall_res2.error().code(), HostFnErrorCode::OutOfGas);
    ASSERT_EQ(gas_consumed2, 10);

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

// NOLINTNEXTLINE(misc-use-internal-linkage)
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
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  handler.start_frame(kPlaceholderInvocation,
                      std::to_underlying(InvokeFlag::None),
                      std::span<uint32_t>{});

  auto [syscall_res, gas_consumed] =
      wrap_outer<NoreturnFailingSyscallMeta>(handler, make_null_context(10000),
                                             kNoTracingSyscall, 0, 0, 0, 0, 0,
                                             0, 0);
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

// NOLINTNEXTLINE(misc-use-internal-linkage)
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
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  handler.start_frame(kPlaceholderInvocation,
                      std::to_underlying(InvokeFlag::None),
                      std::span<uint32_t>{});

  auto [syscall_res, gas_consumed] =
      wrap_outer<NoErrorSyscallMeta>(handler, make_null_context(10000),
                                     kNoTracingSyscall, 0, 0, 0, 0, 0, 0, 0);
  ASSERT_TRUE(syscall_res.has_value());

  auto [syscall_res2, gas_consumed2] =
      wrap_outer<NoErrorSyscallMeta>(handler, make_null_context(10),
                                     kNoTracingSyscall, 0, 0, 0, 0, 0, 0, 0);
  ASSERT_FALSE(syscall_res2.has_value());
  ASSERT_EQ(syscall_res2.error().code(), HostFnErrorCode::OutOfGas);
  ASSERT_EQ(gas_consumed2, 10);
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
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
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  handler.start_frame(kPlaceholderInvocation,
                      std::to_underlying(InvokeFlag::None),
                      std::span<uint32_t>{});

  auto [syscall_res, gas_consumed] =
      wrap_outer<NoReturnNoErrorSyscallMeta>(handler, make_null_context(10000),
                                             kNoTracingSyscall, 0, 0, 0, 0, 0,
                                             0, 0);
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::Exit);

  auto [syscall_res2, gas_consumed2] =
      wrap_outer<NoReturnNoErrorSyscallMeta>(handler, make_null_context(10),
                                             kNoTracingSyscall, 0, 0, 0, 0, 0,
                                             0, 0);
  ASSERT_FALSE(syscall_res2.has_value());
  ASSERT_EQ(syscall_res2.error().code(), HostFnErrorCode::OutOfGas);
  ASSERT_EQ(gas_consumed2, 10);
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
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
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  handler.start_frame(kPlaceholderInvocation,
                      std::to_underlying(InvokeFlag::ReadOnly),
                      std::span<uint32_t>{});

  auto [syscall_res, gas_consumed] =
      wrap_outer<NoReturnReadOnlySyscallMeta>(handler, make_null_context(10000),
                                              kNoTracingSyscall, 0, 0, 0, 0, 0,
                                              0, 0);
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::Exit);
  // Gas is not consumed by the syscall.
  // TODO(geoff): This will break when we add a default gas cost for the error
  // handling (calling a non-readonly syscal in read-only mode should cost at
  // least some gas)
  ASSERT_EQ(gas_consumed, 0);

  auto [syscall_res2, gas_consumed2] =
      wrap_outer<NoReturnReadOnlySyscallMeta>(handler, make_null_context(10),
                                              kNoTracingSyscall, 0, 0, 0, 0, 0,
                                              0, 0);
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::Exit);
  // TODO(geoff): same as above
  ASSERT_EQ(gas_consumed2, 0);
}

// WrappedGasAmount must not be implicitly constructible from int,
// ensuring that accidentally returning a non-gas value as the gas amount
// is a compile error.
static_assert(!std::is_convertible_v<int, WrappedGasAmount>);
static_assert(!std::is_convertible_v<int32_t, WrappedGasAmount>);
static_assert(!std::is_convertible_v<uint64_t, WrappedGasAmount>);
static_assert(!std::is_convertible_v<GasAmount, WrappedGasAmount>);

// Regression test for the bug in syscall_Return_inner where
// the gas amount field was accidentally set to a cast of a (negative)
// SyscallErrorCode instead of 0.
TEST_F(SyscallErrorHandlingTest, ReturnOversizedBufferGasIsZero)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

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
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

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

TEST_F(SyscallErrorHandlingTest,
       WriteAddressRequiredWeightReadOnlyReturnsInvalidSyscallInput)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  handler.start_frame(kPlaceholderInvocation,
                      std::to_underlying(InvokeFlag::ReadOnly),
                      std::span<uint32_t>{});

  NativeSyscallContext ctx(handler, 10000);
  auto status = ctx.syscall<SyscallNo::kWriteAddressRequiredWeight>(Weight{1});
  ASSERT_TRUE(status.has_value());
  ASSERT_EQ(*status, SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr);
}

} // namespace hogd
