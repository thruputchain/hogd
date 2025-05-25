#include <cstring>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/preset_contracts/shared.h"
#include "hogd/hogexec/runtime/multiplexed_context.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "syscall.hh"
#include "test_contracts/integration_test_contracts/fibonacci/fibonacci.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class FibonacciTest : public ::testing::Test,
                      public NativeContractTestFramework {
protected:
  void SetUp() override
  {
    wasm_runtime_context_ = make_hogexec_wasm_context(RuntimeEngine::kWasmi);
  }
  std::shared_ptr<RuntimeContext> wasm_runtime_context_;
};

// Simple test for sdk, to verify that calls actually happen.
TEST_F(FibonacciTest, FibonacciCalls)
{
  auto contract = test::load_test_contract("fibonacci.wasm");
  auto contract_addr = make_contract_address(0);
  in_memory_db_.write_contract(sha256(*contract), contract);
  in_memory_db_.write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto method = ContractTraitMeta<Fibonacci>::method_id<"fibonacci">();
  ASSERT_EQ(method, 96596868703008);

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = contract_addr,
                                   .method = ContractTraitMeta<
                                       Fibonacci>::method_id<"initialize">(),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = ContractTraitMeta<Fibonacci>::
                                       make_calldata<"initialize">()}}},
        wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.result_);
    ASSERT_EQ(*res.result_, 0);
    auto commit_res = handler.try_commit_transaction()();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }

  advance_block();

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = contract_addr,
                                   .method = ContractTraitMeta<
                                       Fibonacci>::method_id<"fibonacci">(),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = ContractTraitMeta<Fibonacci>::
                                       make_calldata<"fibonacci">(5U)}}},
        wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.result_);
    ASSERT_EQ(*res.result_, 0);
    auto commit_res = handler.try_commit_transaction()();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }
}

// Test whether the output of the fibonacci contract is correct.
TEST_F(FibonacciTest, FibonacciCorrectness)
{
  auto contract = test::load_test_contract("fibonacci.wasm");
  auto fib_contract_addr = make_contract_address(0);
  in_memory_db_.write_contract(sha256(*contract), contract);
  in_memory_db_.write_deployment(fib_contract_addr, sha256(*contract));
  setup_grant_storage_token(fib_contract_addr, 1'000'000'000);

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [fib_contract_addr](SystemCallHandler &handler,
                       NativeRuntime &) -> InvokeStatus<int32_t> {
            // Decode the calldata as a u32, then invoke the fibonacci method with that input.
            auto calldata = handler.get_calldata();
            if (calldata.size() != sizeof(uint32_t)) {
              return InvokeError{InvokeErrorCode::kInvokeDeterministic};
            }
            uint32_t val;
            std::memcpy(&val, calldata.data(), sizeof(uint32_t));
            
            auto res = handler.invoke(Invocation{.invoked_address = fib_contract_addr,
                                                 .method = ContractTraitMeta<Fibonacci>::method_id<"fibonacci">(),
                                                 .gas_limit = UINT64_MAX,
                                                 .calldata = ContractTraitMeta<Fibonacci>::make_calldata<"fibonacci">(val)},
                                                 UINT64_MAX,
                                                 0);
            // Check that the invocation succeeds.
            if (!res.first.has_value()) {
              return InvokeError{InvokeErrorCode::kInvokeDeterministic};
            }
            if (res.first.value() != SyscallErrorCode::kNone) {
              return InvokeError{InvokeErrorCode::kInvokeDeterministic};
            }
            // Check that the return buffer is valid.
            auto return_buffer = handler.get_return_buffer();
            if (!return_buffer.has_value()) {
              return InvokeError{InvokeErrorCode::kInvokeDeterministic};
            }
            // decode the return buffer
            auto decoded = ContractTraitMeta<Fibonacci>::decode_return<"fibonacci">(std::as_bytes(std::span(return_buffer.value()->data(), return_buffer.value()->size())));
            if (!decoded.has_value()) {
              return InvokeError{InvokeErrorCode::kInvokeDeterministic};
            }
            return decoded.value();
  
           }},
      });

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> native_contract_map;
  native_contract_map.emplace(0, std::move(native_contract));
  auto native_runtime_context =
      std::make_shared<NativeContext>(std::move(native_contract_map));
    
  auto multiplexed_context = std::make_shared<MultiplexedContext>(
      std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>>{
          {ContractType::kNative, native_runtime_context},
          {ContractType::kWasmi, wasm_runtime_context_}});
  

}

} // namespace hogd
