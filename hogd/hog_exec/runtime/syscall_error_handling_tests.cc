#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "auth_claim.hh"
#include "auth_token.hh"
#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/syscall_entry_outer.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "hogd/hog_exec/runtime/syscall_meta.h"
#include "hogd/hog_exec/runtime/syscall_meta_concept.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize.h"

namespace hogd {

namespace {

template<HostFnErrorCode Code>
std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
error_syscall_func()
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

  constexpr static ExecutionContext make_null_context(GasAmount gas_limit)
  {
    return ExecutionContext{
        .memory = std::span<std::byte>{},
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

    ASSERT_TRUE(handler
                    .start_frame(kPlaceholderInvocation,
                                 std::to_underlying(InvokeFlag::None),
                                 ValidatedAuthTokens{})
                    .has_value());

    auto [syscall_res, gas_consumed] =
        wrap_outer<ErrorSyscallMeta<Code>>(handler, make_null_context(10000),
                                           kNoTracingSyscall, 0, 0, 0, 0, 0, 0,
                                           0)();
    ASSERT_FALSE(syscall_res.has_value());
    ASSERT_EQ(syscall_res.error().code(), Code);

    auto [syscall_res2, gas_consumed2] = wrap_outer<ErrorSyscallMeta<Code>>(
        handler, make_null_context(kSyscallBaseCost + 1), kNoTracingSyscall, 0,
        0, 0, 0, 0, 0, 0)();
    ASSERT_FALSE(syscall_res2.has_value());
    ASSERT_EQ(syscall_res2.error().code(), Code);
    ASSERT_EQ(gas_consumed2, kSyscallBaseCost + 1);

    auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
  }

  template<HostFnErrorCode Code> void run_gas_limit_supercedes_test()
  {
    // The handler and runtime are entirely irrelevant for this test
    // we just need them to invoke the syscall handler and manipulate available
    // gas
    auto context = make_hog_exec_context();
    auto handler = make_new_system_call_handler(Transaction{}, *context);

    ASSERT_TRUE(handler
                    .start_frame(kPlaceholderInvocation,
                                 std::to_underlying(InvokeFlag::None),
                                 ValidatedAuthTokens{})
                    .has_value());

    auto [syscall_res, gas_consumed] =
        wrap_outer<ErrorSyscallMeta<Code>>(handler, make_null_context(10000),
                                           kNoTracingSyscall, 0, 0, 0, 0, 0, 0,
                                           0)();
    ASSERT_FALSE(syscall_res.has_value());
    ASSERT_EQ(syscall_res.error().code(), Code);
    ASSERT_EQ(gas_consumed, 1000 + kSyscallBaseCost);

    auto [syscall_res2, gas_consumed2] = wrap_outer<ErrorSyscallMeta<Code>>(
        handler, make_null_context(kSyscallBaseCost + 1), kNoTracingSyscall, 0,
        0, 0, 0, 0, 0, 0)();
    ASSERT_FALSE(syscall_res2.has_value());
    ASSERT_EQ(syscall_res2.error().code(), HostFnErrorCode::OutOfGas);
    ASSERT_EQ(gas_consumed2, kSyscallBaseCost + 1);

    auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
  }

  // With no budget for the crossing, the body never runs, so it cannot
  // produce a transaction-fatal error. Fatal errors from bodies that do run
  // still take precedence, as run_error_override_test checks above.
  template<HostFnErrorCode Code> void run_fatal_body_at_zero_gas_test()
  {
    auto context = make_hog_exec_context();
    auto handler = make_new_system_call_handler(Transaction{}, *context);

    ASSERT_TRUE(handler
                    .start_frame(kPlaceholderInvocation,
                                 std::to_underlying(InvokeFlag::None),
                                 ValidatedAuthTokens{})
                    .has_value());

    auto [syscall_res, gas_consumed] =
        wrap_outer<ErrorSyscallMeta<Code>>(handler, make_null_context(0),
                                           kNoTracingSyscall, 0, 0, 0, 0, 0, 0,
                                           0)();
    ASSERT_FALSE(syscall_res.has_value());
    ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::OutOfGas);
    ASSERT_EQ(gas_consumed, 0);

    auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
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
noreturn_failing_syscall_func()
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

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  auto [syscall_res, gas_consumed] =
      wrap_outer<NoreturnFailingSyscallMeta>(handler, make_null_context(10000),
                                             kNoTracingSyscall, 0, 0, 0, 0, 0,
                                             0, 0)();
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::Unrecoverable);
}

namespace {

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
noerror_syscall_func()
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

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  auto [syscall_res, gas_consumed] =
      wrap_outer<NoErrorSyscallMeta>(handler, make_null_context(10000),
                                     kNoTracingSyscall, 0, 0, 0, 0, 0, 0, 0)();
  ASSERT_TRUE(syscall_res.has_value());

  auto [syscall_res2, gas_consumed2] =
      wrap_outer<NoErrorSyscallMeta>(handler, make_null_context(10),
                                     kNoTracingSyscall, 0, 0, 0, 0, 0, 0, 0)();
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

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  auto [syscall_res, gas_consumed] =
      wrap_outer<NoReturnNoErrorSyscallMeta>(handler, make_null_context(10000),
                                             kNoTracingSyscall, 0, 0, 0, 0, 0,
                                             0, 0)();
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::Exit);

  auto [syscall_res2, gas_consumed2] =
      wrap_outer<NoReturnNoErrorSyscallMeta>(handler, make_null_context(10),
                                             kNoTracingSyscall, 0, 0, 0, 0, 0,
                                             0, 0)();
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

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::ReadOnly),
                               ValidatedAuthTokens{})
                  .has_value());

  auto [syscall_res, gas_consumed] =
      wrap_outer<NoReturnReadOnlySyscallMeta>(handler, make_null_context(10000),
                                              kNoTracingSyscall, 0, 0, 0, 0, 0,
                                              0, 0)();
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::Exit);
  // Gas is not consumed by the syscall.
  // Refused before the body ran, so nothing is billed for the work -- but the
  // crossing was still paid, which is the "at least some gas" a TODO here used
  // to ask for.
  ASSERT_EQ(gas_consumed, kSyscallBaseCost);

  auto [syscall_res2, gas_consumed2] =
      wrap_outer<NoReturnReadOnlySyscallMeta>(handler, make_null_context(10),
                                              kNoTracingSyscall, 0, 0, 0, 0, 0,
                                              0, 0)();
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::Exit);
  // The crossing again, but this frame had only 10 gas: what is reported back
  // is clamped to what the frame actually had.
  ASSERT_EQ(gas_consumed2, 10);
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

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

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

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

// Test that syscall_Return_inner with a valid buffer returns 0 gas consumed
TEST_F(SyscallErrorHandlingTest, ReturnValidBufferGasIsZero)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  std::array<std::byte, 10> valid_buffer{};

  auto [status, gas] =
      syscall_Return_inner(handler, std::span<const std::byte>(valid_buffer),
                           false, {});

  // The syscall should succeed with Return
  ASSERT_FALSE(status.has_value());
  ASSERT_EQ(status.error().code(), HostFnErrorCode::Return);

  // The gas consumed must be 0
  ASSERT_EQ(gas.get(), 0);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

namespace {

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
bad_alloc_syscall_func()
{
  throw std::bad_alloc{};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
non_std_exception_syscall_func()
{
  throw 42; // NOLINT(hicpp-exception-baseclass)
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
template<auto Func> struct FuncSyscallMeta {
  constexpr static bool kIsReadOnlySyscall = false;
  constexpr static auto kInnerFunc = Func;

  constexpr static SyscallNo kSyscallNo = kNoTracingSyscall;
  constexpr static bool kCanReturn = true;
  constexpr static std::array<std::string_view, 0> kInputNames = {};
  constexpr static std::array<std::string_view, 0> kOutputNames = {};
  constexpr static std::array<SyscallErrorCode, 0> kAllowedErrors = {};
};

static_assert(is_syscall_meta<FuncSyscallMeta<&bad_alloc_syscall_func>>);

namespace {
SyscallInnerResult
unexpected_syscall_body()
{
  ADD_FAILURE() << "A syscall body ran without gas to cover the crossing";
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

SyscallInnerResult
budget_checking_syscall_body(SystemCallHandler & /*handler*/,
                             WrappedGasAmount available)
{
  EXPECT_EQ(available.get(), 17);
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{3}};
}
} // namespace

TEST_F(SyscallErrorHandlingTest, UnaffordableCrossingDoesNotEnterBody)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);
  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  for (GasAmount budget :
       {GasAmount{0}, kSyscallBaseCost - 1, kSyscallBaseCost}) {
    SCOPED_TRACE(budget);
    auto [status, consumed] =
        wrap_outer<FuncSyscallMeta<&unexpected_syscall_body>>(
            handler, make_null_context(budget), kNoTracingSyscall, 0, 0, 0, 0,
            0, 0, 0)();
    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), HostFnErrorCode::OutOfGas);
    EXPECT_EQ(consumed, budget);
  }

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

TEST_F(SyscallErrorHandlingTest, BodyBudgetExcludesCrossingCharge)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);
  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  auto [status, consumed] =
      wrap_outer<FuncSyscallMeta<&budget_checking_syscall_body>>(
          handler, make_null_context(kSyscallBaseCost + 17), kNoTracingSyscall,
          0, 0, 0, 0, 0, 0, 0)();
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(*status, SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_EQ(consumed, kSyscallBaseCost + 3);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

// An exception escaping a syscall body (e.g. std::bad_alloc from an
// allocating storage/cache insert) must be translated to a structured
// Unrecoverable error at the wrap_after_parse boundary instead of
// terminating hogexec (issue #3400 and siblings).
TEST_F(SyscallErrorHandlingTest, ThrowingSyscallTranslatedToUnrecoverable)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  auto [syscall_res, gas_consumed] =
      wrap_outer<FuncSyscallMeta<&bad_alloc_syscall_func>>(
          handler, make_null_context(10000), kNoTracingSyscall, 0, 0, 0, 0, 0,
          0, 0)();
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::Unrecoverable);
  ASSERT_EQ(gas_consumed, kSyscallBaseCost);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

TEST_F(SyscallErrorHandlingTest, NonStdExceptionTranslatedToUnrecoverable)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  auto [syscall_res, gas_consumed] =
      wrap_outer<FuncSyscallMeta<&non_std_exception_syscall_func>>(
          handler, make_null_context(10000), kNoTracingSyscall, 0, 0, 0, 0, 0,
          0, 0)();
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::Unrecoverable);
  ASSERT_EQ(gas_consumed, kSyscallBaseCost);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

TEST_F(SyscallErrorHandlingTest,
       WriteAddressRequiredWeightReadOnlyReturnsInvalidSyscallInput)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::ReadOnly),
                               ValidatedAuthTokens{})
                  .has_value());

  NativeSyscallContext ctx(handler, 10000);
  auto status =
      ctx.syscall<SyscallNo::kWriteAddressRequiredWeight>(Weight{1})();
  ASSERT_TRUE(status.has_value());
  ASSERT_EQ(*status, SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

// ---------------------------------------------------------------------------
// A syscall entered with the frame's gas already exhausted must not succeed.
//
// A guest runtime can enter a syscall with host-visible gas already
// saturated to zero: the gas checkpoint that would have trapped the contract
// sits after the call boundary, not before it, so the first over-budget
// operation the host sees is the syscall itself. wrap_after_parse is the
// backstop -- it rewrites any handleable syscall result to OutOfGas once the
// syscall's cost meets or exceeds the gas remaining at entry.
//
// The comparison is `>=`, and that is load-bearing. Under `>`, a syscall whose
// gas cost is zero -- MintAuthToken and Return both are -- would succeed at
// zero remaining gas, which is the shape reported in #1759: mint a fresh auth
// token in a frame that should already have failed out-of-gas, hand it back
// through Return, and let the parent frame commit and consume a capability the
// child never paid for. The tests below pin the boundary from both sides so
// the comparison cannot be quietly loosened.

namespace {

// The shape of a zero-cost syscall (MintAuthToken, Return, ...): succeeds, and
// reports no gas. This is the case that `>` would let through.
std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
free_success_syscall_func()
{
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

// The shape of syscall_Return_inner: reports no gas, and signals a successful
// frame return through the error channel.
std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
free_return_syscall_func()
{
  return {HostFnError{HostFnErrorCode::Return}, WrappedGasAmount{0}};
}

constexpr GasAmount kFixedSyscallCost = 100;

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
fixed_cost_syscall_func()
{
  return {SyscallErrorCode::kSyscallErrorCodeNone,
          WrappedGasAmount{kFixedSyscallCost}};
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
struct FreeSuccessSyscallMeta {
  constexpr static bool kIsReadOnlySyscall = false;
  constexpr static auto kInnerFunc = &free_success_syscall_func;

  constexpr static SyscallNo kSyscallNo = kNoTracingSyscall;
  constexpr static bool kCanReturn = true;
  constexpr static std::array<std::string_view, 0> kInputNames = {};
  constexpr static std::array<std::string_view, 0> kOutputNames = {};
  constexpr static std::array<SyscallErrorCode, 0> kAllowedErrors = {};
};

// NOLINTNEXTLINE(misc-use-internal-linkage)
struct FreeReturnSyscallMeta {
  constexpr static bool kIsReadOnlySyscall = false;
  constexpr static auto kInnerFunc = &free_return_syscall_func;

  constexpr static SyscallNo kSyscallNo = kNoTracingSyscall;
  constexpr static bool kCanReturn = true;
  constexpr static std::array<std::string_view, 0> kInputNames = {};
  constexpr static std::array<std::string_view, 0> kOutputNames = {};
  constexpr static std::array<SyscallErrorCode, 0> kAllowedErrors = {};
};

// NOLINTNEXTLINE(misc-use-internal-linkage)
struct FixedCostSyscallMeta {
  constexpr static bool kIsReadOnlySyscall = false;
  constexpr static auto kInnerFunc = &fixed_cost_syscall_func;

  constexpr static SyscallNo kSyscallNo = kNoTracingSyscall;
  constexpr static bool kCanReturn = true;
  constexpr static std::array<std::string_view, 0> kInputNames = {};
  constexpr static std::array<std::string_view, 0> kOutputNames = {};
  constexpr static std::array<SyscallErrorCode, 0> kAllowedErrors = {};
};

static_assert(is_syscall_meta<FreeSuccessSyscallMeta>);
static_assert(is_syscall_meta<FreeReturnSyscallMeta>);
static_assert(is_syscall_meta<FixedCostSyscallMeta>);

// A zero-cost syscall is exactly the case that distinguishes `>=` from `>`.
// If this test starts failing, an over-budget contract can mutate frame state
// for free.
TEST_F(SyscallErrorHandlingTest, ZeroCostSyscallAtZeroGasIsOutOfGas)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  auto [syscall_res, gas_consumed] =
      wrap_outer<FreeSuccessSyscallMeta>(handler, make_null_context(0),
                                         kNoTracingSyscall, 0, 0, 0, 0, 0, 0,
                                         0)();
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::OutOfGas);
  ASSERT_EQ(gas_consumed, 0);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

// Just under what the call costs. A zero-cost body still owes the crossing, so
// there is now a band between zero gas and affording the call at all, and the
// same `>=` has to reject all of it. gas_consumed is clamped to the budget:
// the frame cannot be billed for more than it had.
TEST_F(SyscallErrorHandlingTest, ZeroCostSyscallBelowCrossingIsOutOfGas)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  constexpr GasAmount kBudget = kSyscallBaseCost - 1;
  auto [syscall_res, gas_consumed] =
      wrap_outer<FreeSuccessSyscallMeta>(handler, make_null_context(kBudget),
                                         kNoTracingSyscall, 0, 0, 0, 0, 0, 0,
                                         0)();
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::OutOfGas);
  ASSERT_EQ(gas_consumed, kBudget);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

// Exactly what the call costs, which is the inclusive edge: `>=` rejects it and
// `>` would not. Affording a syscall down to its last gas would leave the frame
// at zero having already mutated state, which is the same hazard the zero-gas
// case above pins.
TEST_F(SyscallErrorHandlingTest, ZeroCostSyscallAtCrossingIsOutOfGas)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  auto [syscall_res, gas_consumed] =
      wrap_outer<FreeSuccessSyscallMeta>(handler,
                                         make_null_context(kSyscallBaseCost),
                                         kNoTracingSyscall, 0, 0, 0, 0, 0, 0,
                                         0)();
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::OutOfGas);
  ASSERT_EQ(gas_consumed, kSyscallBaseCost);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

// The second half of the #1759 chain: Return is what carries the child frame's
// outputs into the parent, and it is also zero-cost. At zero gas it must not
// be allowed to report a successful frame return.
TEST_F(SyscallErrorHandlingTest, ZeroCostReturnAtZeroGasIsOutOfGas)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  auto [syscall_res, gas_consumed] =
      wrap_outer<FreeReturnSyscallMeta>(handler, make_null_context(0),
                                        kNoTracingSyscall, 0, 0, 0, 0, 0, 0,
                                        0)();
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::OutOfGas);
  ASSERT_EQ(gas_consumed, 0);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

// The other side of the boundary: the rewrite must not swallow syscalls the
// frame can actually afford, or every zero-cost syscall becomes unusable.
//
// "Zero-cost" is now about the body: it bills nothing, but the call still pays
// kSyscallBaseCost for the crossing, which wrap_after_parse_async adds
// centrally. So the affordable case is one gas above what the call costs, not
// one gas above zero.
TEST_F(SyscallErrorHandlingTest, ZeroCostSyscallSucceedsWithGasRemaining)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  constexpr GasAmount kCallCost = kSyscallBaseCost;
  auto [syscall_res, gas_consumed] =
      wrap_outer<FreeSuccessSyscallMeta>(handler,
                                         make_null_context(kCallCost + 1),
                                         kNoTracingSyscall, 0, 0, 0, 0, 0, 0,
                                         0)();
  ASSERT_TRUE(syscall_res.has_value());
  ASSERT_EQ(*syscall_res, SyscallErrorCode::kSyscallErrorCodeNone);
  ASSERT_EQ(gas_consumed, kCallCost);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

// Pins the comparison itself at a nonzero cost: a syscall that costs exactly
// the gas remaining is not affordable, one gas more is.
TEST_F(SyscallErrorHandlingTest, SyscallGasBoundaryIsInclusive)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  // What the call costs: the body's fixed charge plus the crossing every
  // syscall pays. That total, not the body's share, is where the boundary sits.
  constexpr GasAmount kCallCost = kFixedSyscallCost + kSyscallBaseCost;

  auto [at_cost, gas_at_cost] =
      wrap_outer<FixedCostSyscallMeta>(handler, make_null_context(kCallCost),
                                       kNoTracingSyscall, 0, 0, 0, 0, 0, 0,
                                       0)();
  ASSERT_FALSE(at_cost.has_value());
  ASSERT_EQ(at_cost.error().code(), HostFnErrorCode::OutOfGas);
  ASSERT_EQ(gas_at_cost, kCallCost);

  auto [above_cost, gas_above_cost] =
      wrap_outer<FixedCostSyscallMeta>(handler,
                                       make_null_context(kCallCost + 1),
                                       kNoTracingSyscall, 0, 0, 0, 0, 0, 0,
                                       0)();
  ASSERT_TRUE(above_cost.has_value());
  ASSERT_EQ(*above_cost, SyscallErrorCode::kSyscallErrorCodeNone);
  ASSERT_EQ(gas_above_cost, kCallCost);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

// A runtime trampoline that debits `gas_remaining - gas_consumed` behind an
// `assert!(gas_consumed <= gas_remaining)` depends on wrap_after_parse to make
// that subtraction safe: the gas it reports back is clamped to the gas the
// frame actually had. Uncapped, a syscall entered on an over-consumed store
// would abort the process from inside the FFI boundary.
TEST_F(SyscallErrorHandlingTest, ReportedGasNeverExceedsGasRemaining)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  // error_syscall_func reports 1000 gas regardless of what the frame has left.
  for (GasAmount gas_remaining : {GasAmount{0}, GasAmount{1}, GasAmount{999},
                                  GasAmount{1000}, GasAmount{1001}}) {
    auto [syscall_res, gas_consumed] =
        wrap_outer<ErrorSyscallMeta<HostFnErrorCode::Return>>(
            handler, make_null_context(gas_remaining), kNoTracingSyscall, 0, 0,
            0, 0, 0, 0, 0)();
    ASSERT_FALSE(syscall_res.has_value());
    ASSERT_LE(gas_consumed, gas_remaining)
        << "gas_remaining = " << gas_remaining;
  }

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

// An exhausted frame cannot enter even a body that would produce a fatal
// error. There is no body result to preserve when the crossing is rejected.
TEST_F(SyscallErrorHandlingTest, AbandonBodyAtZeroGasIsNotEntered)
{
  run_fatal_body_at_zero_gas_test<HostFnErrorCode::Abandon>();
}

TEST_F(SyscallErrorHandlingTest, ValidationFailureBodyAtZeroGasIsNotEntered)
{
  run_fatal_body_at_zero_gas_test<HostFnErrorCode::ValidationFailure>();
}

TEST_F(SyscallErrorHandlingTest, UnrecoverableBodyAtZeroGasIsNotEntered)
{
  run_fatal_body_at_zero_gas_test<HostFnErrorCode::Unrecoverable>();
}

// The same boundary, driven through the real syscall implementations rather
// than synthetic metas. MintAuthToken and Return are the two zero-cost
// syscalls #1759 chained together, so they are the ones that have to hold.

TEST_F(SyscallErrorHandlingTest, MintAuthTokenAtZeroGasIsOutOfGas)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  AuthTokenNode root_node{.consumer_address =
                              kPlaceholderInvocation.invoked_address,
                          .payload = x23::serialize(99U),
                          .child_nodes = {}};
  auto serialized_root = x23::serialize(root_node);

  NativeSyscallContext ctx(handler, 0);
  uint32_t token_id = 0;
  auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
      BlockHeight{1000},
      std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                     serialized_root.data()),
                                 serialized_root.size()},
      token_id)();

  ASSERT_FALSE(mint_res.has_value());
  ASSERT_EQ(mint_res.error().code(), HostFnErrorCode::OutOfGas);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

TEST_F(SyscallErrorHandlingTest, ReturnAtZeroGasIsOutOfGas)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  NativeSyscallContext ctx(handler, 0);
  std::array<std::byte, 4> ret_buf{};
  auto ret_res =
      ctx.syscall<SyscallNo::kReturn>(std::span<const std::byte>{ret_buf},
                                      false, std::span<const std::byte>{})();

  // OutOfGas, not Return: Return is what commits the frame to its parent.
  ASSERT_FALSE(ret_res.has_value());
  ASSERT_EQ(ret_res.error().code(), HostFnErrorCode::OutOfGas);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

namespace {

// A syscall body that has to wait before it can answer.  Nothing in production
// suspends yet, so these are what pin wrap_after_parse()'s coroutine path:
// yielding to the thread pool means the wrapper's epilogue runs on a different
// thread than the one that entered the syscall.
x23::Async<SyscallInnerResult>
suspending_syscall_func()
{
  co_await default_threadpool().bg();
  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                               WrappedGasAmount{1000}};
}

// A body that fails after it suspended.  The exception surfaces at
// wrap_after_parse()'s co_await rather than at the call, so it needs the same
// translation to Unrecoverable that a body throwing inline gets.
x23::Async<SyscallInnerResult>
throw_after_suspending()
{
  co_await default_threadpool().bg();
  throw std::bad_alloc();
}

} // namespace

TEST_F(SyscallErrorHandlingTest, SuspendingSyscallBodyRoundTrips)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  auto [syscall_res, gas_consumed] =
      wrap_outer<FuncSyscallMeta<&suspending_syscall_func>>(
          handler, make_null_context(10000), kNoTracingSyscall, 0, 0, 0, 0, 0,
          0, 0)();
  ASSERT_TRUE(syscall_res.has_value());
  ASSERT_EQ(*syscall_res, SyscallErrorCode::kSyscallErrorCodeNone);
  ASSERT_EQ(gas_consumed, 1000 + kSyscallBaseCost);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

// The gas epilogue is the part a suspension could most easily skip past, so
// pin that a body which outspends the frame is still rewritten to OutOfGas
// after resuming.
TEST_F(SyscallErrorHandlingTest, SuspendingSyscallBodyStillMeteredOnResume)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  auto [syscall_res, gas_consumed] =
      wrap_outer<FuncSyscallMeta<&suspending_syscall_func>>(
          handler, make_null_context(kSyscallBaseCost + 1), kNoTracingSyscall,
          0, 0, 0, 0, 0, 0, 0)();
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::OutOfGas);
  ASSERT_EQ(gas_consumed, kSyscallBaseCost + 1);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

TEST_F(SyscallErrorHandlingTest, ThrowAfterSuspendTranslatedToUnrecoverable)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);

  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  auto [syscall_res, gas_consumed] =
      wrap_outer<FuncSyscallMeta<&throw_after_suspending>>(
          handler, make_null_context(10000), kNoTracingSyscall, 0, 0, 0, 0, 0,
          0, 0)();
  ASSERT_FALSE(syscall_res.has_value());
  ASSERT_EQ(syscall_res.error().code(), HostFnErrorCode::Unrecoverable);
  ASSERT_EQ(gas_consumed, kSyscallBaseCost);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

// VerifySignature reserves the length-priced Ed25519 cost before verifying;
// driven through wrap_outer to cover the needs_gas-without-handler path
// (#4172).
TEST_F(SyscallErrorHandlingTest, VerifySignatureReservesGasBeforeVerifying)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);
  ASSERT_TRUE(handler
                  .start_frame(kPlaceholderInvocation,
                               std::to_underlying(InvokeFlag::None),
                               ValidatedAuthTokens{})
                  .has_value());

  // wrap_outer arg order: scheme, then each span as (offset, length). Regions
  // are disjoint and zero-filled, so verification fails only once it runs.
  constexpr GasAmount kPublicKeyAddr = 0;
  constexpr GasAmount kSignatureAddr = 32;
  constexpr GasAmount kMessageAddr = 96;
  constexpr GasAmount kMessageSize = 128;
  std::array<std::byte, kMessageAddr + kMessageSize> memory{};
  const auto scheme = std::to_underlying(CryptographicScheme::Ed25519);
  const GasAmount required = GasCost::verify_signature(kMessageSize);
  const GasAmount cost = kSyscallBaseCost + required;

  // At or below the full cost the frame faults before verifying, charged only
  // what it had.
  for (const GasAmount budget :
       {GasAmount{0}, kSyscallBaseCost, cost - 1, cost}) {
    SCOPED_TRACE(::testing::Message() << "budget=" << budget);
    auto [result, charged] =
        wrap_outer<SyscallMeta<SyscallNo::kVerifySignature>>(
            handler,
            ExecutionContext{.memory = memory, .gas_remaining = budget},
            SyscallNo::kVerifySignature, scheme, kSignatureAddr,
            sizeof(Ed25519Signature), kPublicKeyAddr, sizeof(Ed25519PublicKey),
            kMessageAddr, kMessageSize)();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), HostFnErrorCode::OutOfGas);
    EXPECT_EQ(charged, budget);
  }

  // One unit more, and the verifier runs and is charged the full cost.
  auto [result, charged] = wrap_outer<SyscallMeta<SyscallNo::kVerifySignature>>(
      handler, ExecutionContext{.memory = memory, .gas_remaining = cost + 1},
      SyscallNo::kVerifySignature, scheme, kSignatureAddr,
      sizeof(Ed25519Signature), kPublicKeyAddr, sizeof(Ed25519PublicKey),
      kMessageAddr, kMessageSize)();
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(charged, cost);

  auto _ = handler.finish_frame(InvokeStatus<void>{}, nullptr)();
}

} // namespace hogd
