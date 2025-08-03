#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/multiplexed_context.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "test_contracts/integration_test_contracts/delegate_call_test/delegate_call_test.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class DelegateCallTestFixture : public ::testing::Test,
                                public NativeContractTestFramework {
public:
  DelegateCallTestFixture() : NativeContractTestFramework(false) {}

protected:
  void SetUp() override
  {
    wasm_runtime_context_ = make_hogexec_wasm_context(RuntimeEngine::kWasmi);

    // Load and deploy the delegate_call_test contract
    auto contract = test::load_test_contract("delegate_call_test.wasm");
    in_memory_db_.write_contract(sha256(*contract), contract);
    in_memory_db_.write_deployment(delegate_call_test_addr_, sha256(*contract));
    setup_grant_storage_token(delegate_call_test_addr_, 1'000'000'000);
  }


  ContractAddress delegate_call_test_addr_ = make_contract_address(0);
  ContractAddress native_contract_addr_ = make_contract_address(1);

  std::shared_ptr<RuntimeContext> wasm_runtime_context_;
  constexpr static GasAmount kSufficientGas = 10000;
};

TEST_F(DelegateCallTestFixture, NativeCallContextIsCorrect) {
  using client_t = ContractTraitClient<DelegateCallTest>;
  client_t client(delegate_call_test_addr_);

  // Create a native contract that calls the WASM contract to test context
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [&](SystemCallHandler &handler,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             // Call test_delegate_call_context with delegate call flag
             auto [delegate_call_res, _gas] =
                 client.invoke<"test_delegate_call_context", std::to_underlying(InvokeFlag::kInvokeFlagDelegateCall)>(handler);
             if (!delegate_call_res.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (!delegate_call_res.value().has_value()) {
               ADD_FAILURE() << "test_delegate_call_context should have succeeded";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Call test_normal_invoke_context with normal call
             auto [normal_invoke_res, _gas2] =
                 client.invoke<"test_normal_invoke_context">(handler);
             if (!normal_invoke_res.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (!normal_invoke_res.value().has_value()) {
               ADD_FAILURE() << "test_normal_invoke_context should have succeeded";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // For a native call, both contexts should return the same addresses
             // (the contract's own address for both current_address and msg_sender)
             auto delegate_call_context = delegate_call_res.value().value();
             auto normal_invoke_context = normal_invoke_res.value().value();
             
             // Extract addresses from the tuples
             auto [delegate_current_addr, delegate_msg_sender] = delegate_call_context.value();
             auto [normal_current_addr, normal_msg_sender] = normal_invoke_context.value();
             
             // For delegate call: current_address should be caller's address, msg_sender should be caller's address
             EXPECT_EQ(delegate_current_addr, native_contract_addr_);
             EXPECT_EQ(delegate_msg_sender, native_contract_addr_);
             
             // For normal call: current_address should be target's address, msg_sender should be caller's address
             EXPECT_EQ(normal_current_addr, delegate_call_test_addr_);
             EXPECT_EQ(normal_msg_sender, native_contract_addr_);

             return {};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>>
      native_contract_map;
  native_contract_map.emplace(0, std::move(native_contract));
  auto native_runtime_context =
      std::make_shared<NativeContext>(std::move(native_contract_map));

  // Deploy the native contract
  auto native_contract_xdr = make_native_contract(0);
  in_memory_db_.write_contract(sha256(*native_contract_xdr),
                               native_contract_xdr);
  in_memory_db_.write_deployment(native_contract_addr_,
                                 sha256(*native_contract_xdr));

  auto multiplexed_context = std::make_shared<MultiplexedContext>(
      std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>>{
          {ContractType::kNative, native_runtime_context},
          {ContractType::kWasmi, wasm_runtime_context_}});

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = native_contract_addr_,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = kSufficientGas,
                                 .calldata = {}}}},
      multiplexed_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success()) << "Delegate call context test should succeed";

  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value())
      << "Transaction should commit successfully";
}

} // namespace hogd 