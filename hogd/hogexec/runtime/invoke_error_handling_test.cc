#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class InvokeErrorHandlingTest : public ::testing::TestWithParam<TestDB>,
                                public NativeContractTestFramework {
public:
  InvokeErrorHandlingTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  void SetUp() override {}
};

// Tests to make sure that InvokeError codes are properly passed back to the
// caller.
TEST_P(InvokeErrorHandlingTest, InvokeErrorCodes)
{
  auto inner_func = [](InvokeErrorCode enum_code) -> InvokeStatus<void> {
    // Using a switch, so that the compiler will warn when an enum value is
    // missing
    switch (enum_code) {
    case InvokeErrorCode::None:
      return {};
    case InvokeErrorCode::Deterministic:
      return InvokeError{InvokeErrorCode::Deterministic};
    case InvokeErrorCode::OutOfGas:
      return InvokeError{InvokeErrorCode::OutOfGas};
    case InvokeErrorCode::Abandon:
      return InvokeError{InvokeErrorCode::Abandon};
    case InvokeErrorCode::ValidationFailure:
      return InvokeError{InvokeErrorCode::ValidationFailure};
    case InvokeErrorCode::Unrecoverable:
      return InvokeError{InvokeErrorCode::Unrecoverable};
    }

    // GCC with -Werror in CI doesn't recognize the above switch as exhaustive,
    throw std::runtime_error("Unhandled enum value");
  };

  constexpr ContractAddress kCalleeAddress = make_contract_address(0);
  constexpr ContractAddress kOrchestratorAddress = make_contract_address(1);

  // Callee: methods that return each InvokeError code. Orchestrator: invokes
  // those methods on the callee contract (no self-reentrance).
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_callee;
  fns_callee.emplace(
      0, [&](NativeSyscallContext & /*unused*/) noexcept -> InvokeStatus<void> {
        return inner_func(InvokeErrorCode::None);
      });
  fns_callee.emplace(
      1, [&](NativeSyscallContext & /*unused*/) noexcept -> InvokeStatus<void> {
        return inner_func(InvokeErrorCode::Deterministic);
      });
  fns_callee.emplace(
      2, [&](NativeSyscallContext & /*unused*/) noexcept -> InvokeStatus<void> {
        return inner_func(InvokeErrorCode::OutOfGas);
      });
  fns_callee.emplace(
      3, [&](NativeSyscallContext & /*unused*/) noexcept -> InvokeStatus<void> {
        return inner_func(InvokeErrorCode::Abandon);
      });
  fns_callee.emplace(
      4, [&](NativeSyscallContext & /*unused*/) noexcept -> InvokeStatus<void> {
        return inner_func(InvokeErrorCode::ValidationFailure);
      });
  fns_callee.emplace(
      5, [&](NativeSyscallContext & /*unused*/) noexcept -> InvokeStatus<void> {
        return inner_func(InvokeErrorCode::Unrecoverable);
      });
  auto callee_contract =
      std::make_unique<NativeContract>(std::move(fns_callee));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_orchestrator;
  fns_orchestrator.emplace(
      0,
      [kCalleeAddress](
          NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto r = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = 0,
          };
          return ctx.syscall<SyscallNo::kInvoke>(kCalleeAddress,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r.has_value() ||
            r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Invoke 0 did not return success";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto r2 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 1,
              .gas_limit = UINT64_MAX,
              .flags = 0,
          };
          return ctx.syscall<SyscallNo::kInvoke>(kCalleeAddress,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r2.has_value() ||
            r2.value() !=
                SyscallErrorCode::kSyscallErrorCodeDeterministicWasm) {
          ADD_FAILURE() << "Invoke 1 did not return kDeterministicWasm";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Out of gas in child becomes deterministic (handleable) error in
        // caller
        auto r3 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 2,
              .gas_limit = 12345,
              .flags = 0,
          };
          return ctx.syscall<SyscallNo::kInvoke>(kCalleeAddress,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r3.has_value() ||
            r3.value() !=
                SyscallErrorCode::kSyscallErrorCodeDeterministicWasm) {
          ADD_FAILURE() << "Invoke 2 did not return out of gas";
          return InvokeError{InvokeErrorCode::OutOfGas};
        }

        auto r4 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 3,
              .gas_limit = UINT64_MAX,
              .flags = 0,
          };
          return ctx.syscall<SyscallNo::kInvoke>(kCalleeAddress,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (r4.has_value() ||
            r4.error() != HostFnError{HostFnErrorCode::Abandon}) {
          ADD_FAILURE() << "Invoke 3 did not return abort transaction";
          return InvokeError{InvokeErrorCode::Abandon};
        }

        auto r5 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 4,
              .gas_limit = UINT64_MAX,
              .flags = 0,
          };
          return ctx.syscall<SyscallNo::kInvoke>(kCalleeAddress,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (r5.has_value() ||
            r5.error() != HostFnError{HostFnErrorCode::ValidationFailure}) {
          ADD_FAILURE() << "Invoke 4 did not return validation failure";
          return InvokeError{InvokeErrorCode::ValidationFailure};
        }

        auto r6 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 5,
              .gas_limit = UINT64_MAX,
              .flags = 0,
          };
          return ctx.syscall<SyscallNo::kInvoke>(kCalleeAddress,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (r6.has_value() ||
            r6.error() != HostFnError{HostFnErrorCode::Unrecoverable}) {
          ADD_FAILURE() << "Invoke 5 did not return unrecoverable";
          return InvokeError{InvokeErrorCode::Unrecoverable};
        }

        return {};
      });
  auto orchestrator_contract =
      std::make_unique<NativeContract>(std::move(fns_orchestrator));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(callee_contract));
  contract_map.emplace(1, std::move(orchestrator_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto callee_xdr = make_native_contract(0);
  write_contract(callee_xdr);
  write_deployment(kCalleeAddress, callee_xdr);

  auto orchestrator_xdr = make_native_contract(1);
  write_contract(orchestrator_xdr);
  write_deployment(kOrchestratorAddress, orchestrator_xdr);

  // now invoke the contract
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{
                            .invoked_address = kOrchestratorAddress,
                            .entrypoint = entrypoint_from_method_id(0),
                            .gas_limit = UINT64_MAX,
                            .calldata = {},
                        },
                }},
        *runtime_context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto r = handler.try_commit_transaction();
    ASSERT_EQ(r, ExecResult::ExecResultOK);
  }
}

// Test that invalid InvokeFlag bits are rejected
TEST_P(InvokeErrorHandlingTest, InvalidInvokeFlagsRejected)
{
  auto native_contract_xdr = make_native_contract(0);
  auto native_contract_address = make_contract_address(0);
  auto target_xdr = make_native_contract(1);
  auto target_address = make_contract_address(1);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_target;
  fns_target.emplace(0,
                     [](NativeSyscallContext & /*unused*/) noexcept
                         -> InvokeStatus<void> { return {}; });
  auto target_contract =
      std::make_unique<NativeContract>(std::move(fns_target));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // Valid flags should succeed
        auto r1 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(target_address,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r1.has_value() ||
            r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Valid flag TryInvoke should succeed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto r2 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::ReadOnly),
          };
          return ctx.syscall<SyscallNo::kInvoke>(target_address,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r2.has_value() ||
            r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Valid flag ReadOnly should succeed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Combined valid flags should succeed
        auto r3 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke) |
                       std::to_underlying(InvokeFlag::ReadOnly),
          };
          return ctx.syscall<SyscallNo::kInvoke>(target_address,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r3.has_value() ||
            r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE()
              << "Combined valid flags TryInvoke|ReadOnly should succeed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Invalid flag bit (0x4 = DelegateCall, not yet implemented)
        auto r4 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = 0x4,
          };
          return ctx.syscall<SyscallNo::kInvoke>(target_address,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r4.has_value() ||
            r4.value() !=
                SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
          ADD_FAILURE() << "Flag 0x4 should be rejected";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Arbitrary garbage bits
        auto r5 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = 0xFF,
          };
          return ctx.syscall<SyscallNo::kInvoke>(target_address,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r5.has_value() ||
            r5.value() !=
                SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
          ADD_FAILURE() << "Garbage flag bits 0xFF should be rejected";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // High bits set
        auto r6 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = 0x7FFF'FFFF,
          };
          return ctx.syscall<SyscallNo::kInvoke>(target_address,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r6.has_value() ||
            r6.value() !=
                SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
          ADD_FAILURE() << "High flag bits should be rejected";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        return {};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(target_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));
  write_contract(native_contract_xdr);
  write_deployment(native_contract_address, native_contract_xdr);
  write_contract(target_xdr);
  write_deployment(target_address, target_xdr);

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{
                            .invoked_address = native_contract_address,
                            .entrypoint = entrypoint_from_method_id(0),
                            .gas_limit = UINT64_MAX,
                            .calldata = {},
                        },
                }},
        *runtime_context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto r = handler.try_commit_transaction();
    ASSERT_EQ(r, ExecResult::ExecResultOK);
  }
}

TEST_P(InvokeErrorHandlingTest, EmptyOwnedAuthTokenSpan)
{
  constexpr ContractAddress kCalleeAddress = make_contract_address(0);
  constexpr ContractAddress kCallerAddress = make_contract_address(1);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_callee2;
  fns_callee2.emplace(0,
                      [](NativeSyscallContext & /*unused*/) noexcept
                          -> InvokeStatus<void> { return {}; });
  auto callee_contract =
      std::make_unique<NativeContract>(std::move(fns_callee2));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller2;
  fns_caller2.emplace(
      0,
      [kCalleeAddress](
          NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = std::to_underlying(InvokeFlag::None),
        };
        auto r = ctx.syscall<SyscallNo::kInvoke>(kCalleeAddress,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        if (!r.has_value() ||
            r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Invoke with an empty auth-token span failed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller2));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(callee_contract));
  contract_map.emplace(1, std::move(caller_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto callee_xdr = make_native_contract(0);
  write_contract(callee_xdr);
  write_deployment(kCalleeAddress, callee_xdr);

  auto caller_xdr = make_native_contract(1);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
}

INSTANTIATE_TEST_SUITE_P(TestDBs, InvokeErrorHandlingTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
