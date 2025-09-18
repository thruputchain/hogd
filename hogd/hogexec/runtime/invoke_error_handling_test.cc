#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "rpc_types.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

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
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [&](SystemCallHandler & /*unused*/,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             return inner_func(InvokeErrorCode::kInvokeNone);
           }},
          {1,
           [&](SystemCallHandler & /*unused*/,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             return inner_func(InvokeErrorCode::kInvokeDeterministic);
           }},
          {2,
           [&](SystemCallHandler & /*unused*/,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             return inner_func(InvokeErrorCode::kInvokeOutOfGas);
           }},
          {3,
           [&](SystemCallHandler & /*unused*/,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             return inner_func(InvokeErrorCode::kInvokeAbortTransaction);
           }},
          {4,
           [&](SystemCallHandler & /*unused*/,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             return inner_func(InvokeErrorCode::kInvokeValidationFailure);
           }},
          {5,
           [&](SystemCallHandler & /*unused*/,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             return inner_func(InvokeErrorCode::kInvokeUnrecoverable);
           }},
          {6,
           [&](SystemCallHandler &handler,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto [r, _gas] =
                 handler.invoke(native_contract_address, 0, UINT64_MAX, {}, 0);
             if (!r.has_value() ||
                 r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               ADD_FAILURE() << "Invoke 0 did not return success";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             auto [r2, _gas2] =
                 handler.invoke(native_contract_address, 1, UINT64_MAX, {}, 0);
             if (!r2.has_value() ||
                 r2.value() !=
                     SyscallErrorCode::kSyscallErrorCodeDeterministicWasm) {
               ADD_FAILURE() << "Invoke 1 did not return kDeterministicWasm";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Out of gas in child becomes deterministic (handleable) error in
             // caller
             auto [r3, _gas3] =
                 handler.invoke(native_contract_address, 2, UINT64_MAX, {}, 0);
             if (!r3.has_value() ||
                 r3.value() !=
                     SyscallErrorCode::kSyscallErrorCodeDeterministicWasm) {
               ADD_FAILURE() << "Invoke 2 did not return out of gas";
               return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
             }

             auto [r4, _gas4] =
                 handler.invoke(native_contract_address, 3, UINT64_MAX, {}, 0);
             if (r4.has_value() ||
                 r4.error() !=
                     HostFnError{HostFnErrorCode::kHostAbortTransaction}) {
               ADD_FAILURE() << "Invoke 3 did not return abort transaction";
               return InvokeError{InvokeErrorCode::kInvokeAbortTransaction};
             }

             auto [r5, _gas5] =
                 handler.invoke(native_contract_address, 4, UINT64_MAX, {}, 0);
             if (r5.has_value() ||
                 r5.error() !=
                     HostFnError{HostFnErrorCode::kHostValidationFailure}) {
               ADD_FAILURE() << "Invoke 4 did not return validation failure";
               return InvokeError{InvokeErrorCode::kInvokeValidationFailure};
             }

             auto [r6, _gas6] =
                 handler.invoke(native_contract_address, 5, UINT64_MAX, {}, 0);
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
      std::make_shared<NativeContext>(std::move(contract_map));
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
        runtime_context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto r = handler.try_commit_transaction();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(*r);
  }
}

INSTANTIATE_TEST_SUITE_P(TestDBs, InvokeErrorHandlingTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
