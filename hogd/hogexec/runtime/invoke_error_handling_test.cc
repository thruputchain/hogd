#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "rpc_types.hh"
#include "syscall_error.hh"
#include "transaction.hh"

namespace hogd {

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
    case InvokeErrorCode::kInvokeNone:
      return {};
    case InvokeErrorCode::kInvokeDeterministic:
      return InvokeError{InvokeErrorCode::kInvokeDeterministic};
    case InvokeErrorCode::kInvokeOutOfGas:
      return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
    case InvokeErrorCode::kInvokeAbortTransaction:
      return InvokeError{InvokeErrorCode::kInvokeAbortTransaction};
    case InvokeErrorCode::kInvokeValidationFailure:
      return InvokeError{InvokeErrorCode::kInvokeValidationFailure};
    case InvokeErrorCode::kInvokeUnrecoverable:
      return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
    }

    // GCC with -Werror in CI doesn't recognize the above switch as exhaustive,
    throw std::runtime_error("Unhandled enum value");
  };

  auto native_contract_xdr = make_native_contract(0);
  auto native_contract_address = make_contract_address(0);

  // a native contract with methods that just return different InvokeError
  // codes, and one method that calls all the other methods.
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [&](NativeSyscallContext & /*unused*/) -> InvokeStatus<void> {
             return inner_func(InvokeErrorCode::kInvokeNone);
           }},
          {1,
           [&](NativeSyscallContext & /*unused*/) -> InvokeStatus<void> {
             return inner_func(InvokeErrorCode::kInvokeDeterministic);
           }},
          {2,
           [&](NativeSyscallContext & /*unused*/) -> InvokeStatus<void> {
             return inner_func(InvokeErrorCode::kInvokeOutOfGas);
           }},
          {3,
           [&](NativeSyscallContext & /*unused*/) -> InvokeStatus<void> {
             return inner_func(InvokeErrorCode::kInvokeAbortTransaction);
           }},
          {4,
           [&](NativeSyscallContext & /*unused*/) -> InvokeStatus<void> {
             return inner_func(InvokeErrorCode::kInvokeValidationFailure);
           }},
          {5,
           [&](NativeSyscallContext & /*unused*/) -> InvokeStatus<void> {
             return inner_func(InvokeErrorCode::kInvokeUnrecoverable);
           }},
          {6,
           [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto [r, _gas] = syscall_invoke(ctx, native_contract_address, 0,
                                                   UINT64_MAX, {}, 0);
             if (!r.has_value() ||
                 r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               ADD_FAILURE() << "Invoke 0 did not return success";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             auto [r2, _gas2] = syscall_invoke(ctx, native_contract_address, 1,
                                                     UINT64_MAX, {}, 0);
             if (!r2.has_value() ||
                 r2.value() !=
                     SyscallErrorCode::kSyscallErrorCodeDeterministicWasm) {
               ADD_FAILURE() << "Invoke 1 did not return kDeterministicWasm";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Out of gas in child becomes deterministic (handleable) error in
             // caller
             auto [r3, _gas3] = syscall_invoke(ctx, native_contract_address, 2,
                                                     UINT64_MAX, {}, 0);
             if (!r3.has_value() ||
                 r3.value() !=
                     SyscallErrorCode::kSyscallErrorCodeDeterministicWasm) {
               ADD_FAILURE() << "Invoke 2 did not return out of gas";
               return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
             }

             auto [r4, _gas4] = syscall_invoke(ctx, native_contract_address, 3,
                                                     UINT64_MAX, {}, 0);
             if (r4.has_value() ||
                 r4.error() !=
                     HostFnError{HostFnErrorCode::kHostAbortTransaction}) {
               ADD_FAILURE() << "Invoke 3 did not return abort transaction";
               return InvokeError{InvokeErrorCode::kInvokeAbortTransaction};
             }

             auto [r5, _gas5] = syscall_invoke(ctx, native_contract_address, 4,
                                                     UINT64_MAX, {}, 0);
             if (r5.has_value() ||
                 r5.error() !=
                     HostFnError{HostFnErrorCode::kHostValidationFailure}) {
               ADD_FAILURE() << "Invoke 4 did not return validation failure";
               return InvokeError{InvokeErrorCode::kInvokeValidationFailure};
             }

             auto [r6, _gas6] = syscall_invoke(ctx, native_contract_address, 5,
                                                     UINT64_MAX, {}, 0);
             if (r6.has_value() ||
                 r6.error() !=
                     HostFnError{HostFnErrorCode::kHostUnrecoverable}) {
               ADD_FAILURE() << "Invoke 5 did not return unrecoverable";
               return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
             }

             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));
  // mark the contract as existing and deployed within the database
  write_contract(native_contract_xdr);
  write_deployment(native_contract_address, native_contract_xdr);

  // now invoke the contract
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{
                            .invoked_address = native_contract_address,
                            .entrypoint = entrypoint_from_method_id(6),
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

  auto target_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext & /*unused*/) -> InvokeStatus<void> {
             return {};
           }}});

  auto caller_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             // Valid flags should succeed
             auto [r1, _g1] =
                 syscall_invoke(ctx, target_address, 0, UINT64_MAX, {},
                                      std::to_underlying(
                                          InvokeFlag::kInvokeFlagTryInvoke));
             if (!r1.has_value() ||
                 r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               ADD_FAILURE() << "Valid flag TryInvoke should succeed";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             auto [r2, _g2] =
                 syscall_invoke(ctx, target_address, 0, UINT64_MAX, {},
                                      std::to_underlying(
                                          InvokeFlag::kInvokeFlagReadOnly));
             if (!r2.has_value() ||
                 r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               ADD_FAILURE() << "Valid flag ReadOnly should succeed";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Combined valid flags should succeed
             auto [r3, _g3] = syscall_invoke(ctx, 
                 target_address, 0, UINT64_MAX, {},
                 std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke) |
                     std::to_underlying(InvokeFlag::kInvokeFlagReadOnly));
             if (!r3.has_value() ||
                 r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               ADD_FAILURE()
                   << "Combined valid flags TryInvoke|ReadOnly should succeed";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Invalid flag bit (0x4 = DelegateCall, not yet implemented)
             auto [r4, _g4] =
                 syscall_invoke(ctx, target_address, 0, UINT64_MAX, {}, 0x4);
             if (!r4.has_value() ||
                 r4.value() !=
                     SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
               ADD_FAILURE() << "Flag 0x4 should be rejected";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Arbitrary garbage bits
             auto [r5, _g5] =
                 syscall_invoke(ctx, target_address, 0, UINT64_MAX, {}, 0xFF);
             if (!r5.has_value() ||
                 r5.value() !=
                     SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
               ADD_FAILURE() << "Garbage flag bits 0xFF should be rejected";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // High bits set
             auto [r6, _g6] = syscall_invoke(ctx, target_address, 0,
                                                   UINT64_MAX, {}, 0x7FFF'FFFF);
             if (!r6.has_value() ||
                 r6.value() !=
                     SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
               ADD_FAILURE() << "High flag bits should be rejected";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }}});

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

INSTANTIATE_TEST_SUITE_P(TestDBs, InvokeErrorHandlingTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
