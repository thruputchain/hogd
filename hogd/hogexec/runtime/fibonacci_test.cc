#include <cstring>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "bcs/decode.h"
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
public:
  FibonacciTest() : NativeContractTestFramework(false) {}

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

  using fib_client_t = ContractTraitClient<Fibonacci>;
  fib_client_t client(contract_addr);

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{.invocation = client.make_invocation<"initialize">(
                                UINT64_MAX)}},
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
            .tx = Transaction{.invocation = client.make_invocation<"fibonacci">(
                                  UINT64_MAX, 5U)}},
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
  // deploy the fibonacci contract
  auto contract = test::load_test_contract("fibonacci.wasm");
  auto fib_contract_addr = make_contract_address(0);
  in_memory_db_.write_contract(sha256(*contract), contract);
  in_memory_db_.write_deployment(fib_contract_addr, sha256(*contract));
  setup_grant_storage_token(fib_contract_addr, 1'000'000'000);

  using fib_client_t = ContractTraitClient<Fibonacci>;

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,

           [fib_contract_addr](SystemCallHandler &handler,
                               NativeRuntime &) -> InvokeStatus<int32_t> {
             // Decode the calldata as two u32s, then invoke the fibonacci
             // method with the first input.  The second is the expected output.
             auto c = bcs::decode_from<uint32_t, uint32_t>(
                 std::as_bytes(std::span(handler.get_calldata())));
             if (!c.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             auto [input, expected_output] = c.value();

             fib_client_t client(fib_contract_addr);

             auto [decoded, _gas] =
                 client.template invoke<"fibonacci">(handler, input);

             // decoded is a HostFnStatus<SyscallStatus<uint32_t>>
             if (!decoded.has_value() || !decoded.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (decoded.value().value() != expected_output) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return 0;
           }},
      });

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>>
      native_contract_map;
  native_contract_map.emplace(0, std::move(native_contract));
  auto native_runtime_context =
      std::make_shared<NativeContext>(std::move(native_contract_map));

  // deploy the native contract
  auto native_contract_xdr = make_native_contract(0);
  auto native_contract_addr = make_contract_address(1);
  in_memory_db_.write_contract(sha256(*native_contract_xdr),
                               native_contract_xdr);
  in_memory_db_.write_deployment(native_contract_addr,
                                 sha256(*native_contract_xdr));

  auto multiplexed_context = std::make_shared<MultiplexedContext>(
      std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>>{
          {ContractType::kNative, native_runtime_context},
          {ContractType::kWasmi, wasm_runtime_context_}});

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = fib_contract_addr,
                                   .method = ContractTraitMeta<
                                       Fibonacci>::method_id<"initialize">(),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = ContractTraitMeta<Fibonacci>::
                                       make_calldata<"initialize">()}}},
        multiplexed_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.result_);
    ASSERT_EQ(*res.result_, 0);
    auto commit_res = handler.try_commit_transaction()();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }

  advance_block();

  {
    // fibonacci(10) = 55
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = native_contract_addr,
                                   .method = 0,
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {10, 0, 0, 0, 55, 0, 0, 0}}}},
        multiplexed_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.result_);
    ASSERT_EQ(*res.result_, 0);
    auto commit_res = handler.try_commit_transaction()();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }

  {
    // fibonacci(40) = 102334155 (0x6197ECB)
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address =
                                                 native_contract_addr,
                                             .method = 0,
                                             .gas_limit = UINT64_MAX,
                                             .calldata = {40, 0, 0, 0, 0xCB,
                                                          0x7E, 0x19, 0x06}}}},
        multiplexed_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.result_);
    ASSERT_EQ(*res.result_, 0);
    auto commit_res = handler.try_commit_transaction()();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }
}

} // namespace hogd
