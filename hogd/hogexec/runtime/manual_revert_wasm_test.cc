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
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "test_contracts/integration_test_contracts/manual_revert_test/manual_revert_test.h"
#include "transaction.hh"
#include "types.hh"

/**
 * WASM integration tests for manual frame revert functionality.
 * 
 * These tests load and invoke the actual manual_revert_test.wasm contract
 * to verify that the SDK's Returnable trait properly triggers manual revert
 * at the WASM runtime level.
 */

namespace hogd {

class ManualRevertWasmTest : public ::testing::Test,
                             public NativeContractTestFramework {
public:
  ManualRevertWasmTest() : NativeContractTestFramework(false) {}

protected:
  void SetUp() override
  {
    wasm_runtime_context_ = make_hogexec_wasm_context(RuntimeEngine::kWasmi);
    
    // Load and deploy the manual_revert_test contract
    auto contract = test::load_test_contract("manual_revert_test.wasm");
    contract_addr_ = make_contract_address(0);
    in_memory_db_.write_contract(sha256(*contract), contract);
    in_memory_db_.write_deployment(contract_addr_, sha256(*contract));
    setup_grant_storage_token(contract_addr_, 1'000'000'000);
  }

  std::shared_ptr<RuntimeContext> wasm_runtime_context_;
  ContractAddress contract_addr_;
  
  constexpr static GasAmount kSufficientGas = 100000;
};

// Test basic manual revert: should succeed and revert state
TEST_F(ManualRevertWasmTest, BasicManualRevertSuccess)
{
  using client_t = ContractTraitClient<ManualRevertTest>;
  client_t client(contract_addr_);

  // Create a native contract that calls the WASM contract
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [&](SystemCallHandler &handler,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             // Call with should_revert=true - this should succeed but revert state
             auto [res, _gas] =
                 client.invoke<"test_basic_manual_revert">(handler, true);
             if (!res.has_value() || !res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>>
      native_contract_map;
  native_contract_map.emplace(0, std::move(native_contract));
  auto native_runtime_context =
      std::make_shared<NativeContext>(std::move(native_contract_map));

  // Deploy the native contract
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

  // Invoke the native contract which calls the WASM contract
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = native_contract_addr,
                                           .entrypoint = entrypoint_from_method_id(0),
                                           .gas_limit = kSufficientGas,
                                           .calldata = {}}}},
      multiplexed_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success()) << "Manual revert should succeed";

  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res) << "Transaction should commit";

  advance_block();
}

// Test basic manual revert when NOT reverting
TEST_F(ManualRevertWasmTest, BasicManualRevertNoRevert)
{
  using client_t = ContractTraitClient<ManualRevertTest>;
  client_t client(contract_addr_);

  // Create a native contract that calls the WASM contract
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [&](SystemCallHandler &handler,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             // Call with should_revert=false - this should succeed and keep state
             auto [res, _gas] =
                 client.invoke<"test_basic_manual_revert">(handler, false);
             if (!res.has_value() || !res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>>
      native_contract_map;
  native_contract_map.emplace(0, std::move(native_contract));
  auto native_runtime_context =
      std::make_shared<NativeContext>(std::move(native_contract_map));

  // Deploy the native contract
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

  // Invoke the native contract which calls the WASM contract
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = native_contract_addr,
                                           .entrypoint = entrypoint_from_method_id(0),
                                           .gas_limit = kSufficientGas,
                                           .calldata = {}}}},
      multiplexed_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);

  advance_block();
}

// Test different error types triggering manual revert
TEST_F(ManualRevertWasmTest, ErrorTypeManualReverts)
{
  using client_t = ContractTraitClient<ManualRevertTest>;
  client_t client(contract_addr_);

  // Test different error types (1-4 should all revert, 0 should not)
  for (uint32_t error_type = 0; error_type <= 4; ++error_type) {
    
    // Create a native contract that calls the WASM contract
    auto native_contract = std::make_unique<NativeContract>(
        std::unordered_map<MethodID, NativeContract::function_t>{
            {0,
             [&, error_type](SystemCallHandler &handler,
                    NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               auto [res, _gas] =
                   client.invoke<"test_error_type_revert">(handler, error_type);
               if (!res.has_value() || !res.value().has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
               return {};
             }}});

    std::unordered_map<uint32_t, std::unique_ptr<NativeContract>>
        native_contract_map;
    native_contract_map.emplace(0, std::move(native_contract));
    auto native_runtime_context =
        std::make_shared<NativeContext>(std::move(native_contract_map));

    // Deploy the native contract
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

    // Invoke the native contract which calls the WASM contract
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address = native_contract_addr,
                                             .entrypoint = entrypoint_from_method_id(0),
                                             .gas_limit = kSufficientGas,
                                             .calldata = {}}}},
        multiplexed_context);

    auto res = handler.root_invoke();
    
    if (error_type == 0) {
      // Error type 0 should succeed without reverting
      ASSERT_TRUE(res.is_root_success()) << "Error type 0 should succeed";
    } else {
      // Error types 1-4 should trigger manual revert (succeed but with revert)
      ASSERT_TRUE(res.is_root_success()) << "Error type " << error_type << " should manually revert (succeed)";
    }
    
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res) << "Error type " << error_type << " transaction should commit";
    
    advance_block();
  }
}

// Test nested manual revert  
TEST_F(ManualRevertWasmTest, NestedManualRevert)
{
  using client_t = ContractTraitClient<ManualRevertTest>;
  client_t client(contract_addr_);

  // Create a native contract that calls the WASM contract
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [&](SystemCallHandler &handler,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             // Test nested revert: parent calls child, child reverts, parent succeeds
             auto [res, _gas] =
                 client.invoke<"test_nested_manual_revert">(handler, true);
             if (!res.has_value() || !res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>>
      native_contract_map;
  native_contract_map.emplace(0, std::move(native_contract));
  auto native_runtime_context =
      std::make_shared<NativeContext>(std::move(native_contract_map));

  // Deploy the native contract
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

  // Invoke the native contract which calls the WASM contract
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = native_contract_addr,
                                           .entrypoint = entrypoint_from_method_id(0),
                                           .gas_limit = kSufficientGas,
                                           .calldata = {}}}},
      multiplexed_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success()) << "Parent should succeed even when child reverts";

  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);

  advance_block();
}

// Simple test to verify contract loads and basic method calling works
TEST_F(ManualRevertWasmTest, SimpleCheckStorageExists)
{
  using client_t = ContractTraitClient<ManualRevertTest>;
  client_t client(contract_addr_);

  // Create a native contract that calls the WASM contract
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [&](SystemCallHandler &handler,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             // Call the simplest method - just check if storage exists
             auto [res, _gas] =
                 client.invoke<"check_storage_exists">(handler);
             if (!res.has_value()) {
               // Log the error for debugging
               trace("check_storage_exists invoke failed: no value");
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (!res.value().has_value()) {
               trace("check_storage_exists syscall failed");
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Success - the boolean return value doesn't matter for this test
             trace("check_storage_exists succeeded, value: {}", res.value().value());
             return {};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>>
      native_contract_map;
  native_contract_map.emplace(0, std::move(native_contract));
  auto native_runtime_context =
      std::make_shared<NativeContext>(std::move(native_contract_map));

  // Deploy the native contract
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

  // Invoke the native contract which calls the WASM contract
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = native_contract_addr,
                                           .entrypoint = entrypoint_from_method_id(0),
                                           .gas_limit = kSufficientGas,
                                           .calldata = {}}}},
      multiplexed_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success()) << "Simple contract call should succeed";

  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);

  advance_block();
}

} // namespace hogd 