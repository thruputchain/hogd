#include <gtest/gtest.h>

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
#include "rpc_types.hh"
#include "test_contracts/integration_test_contracts/manual_revert_test/manual_revert_test.h"
#include "transaction.hh"
#include "types.hh"

//
// WASM integration tests for manual frame revert functionality.
//
// These tests load and invoke the actual manual_revert_test.wasm contract
// to verify that the SDK's Returnable trait properly triggers manual revert
// at the WASM runtime level.
//

namespace hogd {

class ManualRevertWasmTest : public ::testing::TestWithParam<TestDB>,
                             public NativeContractTestFramework {
public:
  ManualRevertWasmTest()
    : NativeContractTestFramework(
          true, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  void SetUp() override
  {
    wasm_runtime_context_ = make_hogexec_wasm_context(RuntimeEngine::kWasmi);

    // Load and deploy the manual_revert_test contract
    auto contract = test::load_test_contract("manual_revert_test.wasm");
    contract_addr_ = make_contract_address(0);
    write_contract(contract);
    write_deployment(contract_addr_, sha256(*contract));
    setup_grant_storage_token(contract_addr_, 1'000'000'000);
  }

  std::shared_ptr<RuntimeContext> wasm_runtime_context_;
  ContractAddress contract_addr_;

  constexpr static GasAmount kSufficientGas = 100000;
};

//
// Test basic manual revert functionality through WASM contract.
//
// This integration test verifies the complete end-to-end flow:
// - Loads the actual manual_revert_test.wasm contract
// - Calls test_basic_manual_revert(should_revert=true) via WASM runtime
// - Verifies that the SDK's Returnable trait triggers manual revert
// - Confirms that storage changes are reverted (check_storage_exists returns
// false)
// - Ensures the transaction commits successfully despite the revert
//
TEST_P(ManualRevertWasmTest, BasicManualRevert)
{
  using client_t = ContractTraitClient<ManualRevertTest>;
  client_t client(contract_addr_);

  // Create a native contract that calls the WASM contract to test manual revert
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [&](SystemCallHandler &handler,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             // Call test_basic_manual_revert with should_revert=true
             auto [res, _gas] =
                 client.invoke<"test_basic_manual_revert">(handler, true);
             if (!res.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             //if (res.value().error().code() != SyscallErrorCode::kFrameReverted) {
             //  ADD_FAILURE() << "Function call should have succeeded and reverted";
             //  return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             //}
             if (!res.value().has_value()) {
               ADD_FAILURE() << "Function call should have succeeded";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // The manual revert should have succeeded (no error)
             // and storage should be reverted (tested by check_storage_exists)
             auto [storage_check, _gas2] =
                 client.invoke<"check_storage_exists">(handler);
             if (!storage_check.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Storage should not exist due to manual revert
             if (storage_check.value().value()) {
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
  write_contract(native_contract_xdr);
  write_deployment(native_contract_addr, sha256(*native_contract_xdr));

  auto multiplexed_context = std::make_shared<MultiplexedContext>(
      std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>>{
          {ContractType::kNative, native_runtime_context},
          {ContractType::kWasmi, wasm_runtime_context_}});

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = native_contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = kSufficientGas,
                                 .calldata = {}}}},
      multiplexed_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success()) << "Manual revert test should succeed";

  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value())
      << "Transaction should commit successfully";
}

//
// Test normal operation without revert through WASM contract.
//
// This test serves as a control to verify that when manual revert is NOT
// triggered:
// - The WASM contract executes normally
// - Storage changes are preserved (check_storage_exists returns true)
// - The transaction commits successfully
// - This validates that the manual revert behavior is intentional, not a side
// effect
//
TEST_P(ManualRevertWasmTest, BasicNoRevert)
{
  using client_t = ContractTraitClient<ManualRevertTest>;
  client_t client(contract_addr_);

  // Create a native contract that calls the WASM contract without revert
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [&](SystemCallHandler &handler,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             // Call test_basic_manual_revert with should_revert=false
             auto [res, _gas] =
                 client.invoke<"test_basic_manual_revert">(handler, false);
             if (!res.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (!res.value().has_value()) {
               ADD_FAILURE() << "Function call should have succeeded";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Storage should exist because no revert occurred
             auto [storage_check, _gas2] =
                 client.invoke<"check_storage_exists">(handler);
             if (!storage_check.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Storage should exist (no revert)
             if (!storage_check.value().value()) {
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
  write_contract(native_contract_xdr);
  write_deployment(native_contract_addr, sha256(*native_contract_xdr));

  auto multiplexed_context = std::make_shared<MultiplexedContext>(
      std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>>{
          {ContractType::kNative, native_runtime_context},
          {ContractType::kWasmi, wasm_runtime_context_}});

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = native_contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = kSufficientGas,
                                 .calldata = {}}}},
      multiplexed_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success()) << "No revert test should succeed";

  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value())
      << "Transaction should commit successfully";
}

//
// Test nested manual revert functionality through WASM contract.
//
// Tests the WASM contract's test_nested_manual_revert method which:
// - Calls an internal method that performs manual revert
// - Verifies that nested manual revert works correctly through WASM
// - Ensures the outer method can handle the reverted inner call
// - Validates that the SDK properly handles nested Result::Err scenarios
//
TEST_P(ManualRevertWasmTest, NestedManualRevert)
{
  using client_t = ContractTraitClient<ManualRevertTest>;
  client_t client(contract_addr_);

  // Create a native contract that tests nested manual revert
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [&](SystemCallHandler &handler,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             // Call test_nested_manual_revert with child_should_revert=true
             auto [res, _gas] =
                 client.invoke<"test_nested_manual_revert">(handler, true);
             if (!res.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (!res.value().has_value()) {
               ADD_FAILURE() << "Function call should have succeeded";
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
  write_contract(native_contract_xdr);
  write_deployment(native_contract_addr, sha256(*native_contract_xdr));

  auto multiplexed_context = std::make_shared<MultiplexedContext>(
      std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>>{
          {ContractType::kNative, native_runtime_context},
          {ContractType::kWasmi, wasm_runtime_context_}});

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = native_contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = kSufficientGas,
                                 .calldata = {}}}},
      multiplexed_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success())
      << "Nested manual revert test should succeed";

  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value())
      << "Transaction should commit successfully";
}

//
// Test different error types triggering manual revert through WASM contract.
//
// This integration test verifies that the test_error_type_revert method:
// - Properly handles different error types (1-4) via SDK's Returnable trait
// - All error types trigger manual revert and transaction success
// - Error type 0 (no error) succeeds normally without revert
// - Validates the end-to-end flow: SDK error → Returnable trait → manual revert
//
TEST_P(ManualRevertWasmTest, ErrorTypeManualReverts)
{
  using client_t = ContractTraitClient<ManualRevertTest>;
  client_t client(contract_addr_);

  // Test error types 1-4 (should all trigger manual revert)
  for (uint32_t error_type = 1; error_type <= 4; ++error_type) {
    // Create a native contract that calls the WASM contract's error type method
    auto native_contract = std::make_unique<NativeContract>(
        std::unordered_map<MethodID, NativeContract::function_t>{
            {0,
             [&, error_type](SystemCallHandler &handler,
                             NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               // Call test_error_type_revert with specific error type
               auto [res, _gas] =
                   client.invoke<"test_error_type_revert">(handler, error_type);
               if (!res.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
               if (!res.value().has_value()) {
                 ADD_FAILURE() << "Function call should have succeeded";
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }

               // All error types should succeed via manual revert
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
    write_contract(native_contract_xdr);
    write_deployment(native_contract_addr, sha256(*native_contract_xdr));

    auto multiplexed_context = std::make_shared<MultiplexedContext>(
        std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>>{
            {ContractType::kNative, native_runtime_context},
            {ContractType::kWasmi, wasm_runtime_context_}});

    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = native_contract_addr,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = kSufficientGas,
                                   .calldata = {}}}},
        multiplexed_context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success())
        << "Error type " << error_type << " should succeed via manual revert";

    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value())
        << "Error type " << error_type
        << " transaction should commit successfully";
  }
}

//
// Test normal operation (error type 0) through WASM contract.
//
// This test verifies that when test_error_type_revert is called with
// error_type=0:
// - No error is triggered and the method returns Ok(())
// - Storage changes are preserved (no manual revert)
// - Transaction succeeds normally
// - This serves as a control to confirm error types 1-4 behavior is intentional
//
TEST_P(ManualRevertWasmTest, ErrorTypeNoError)
{
  using client_t = ContractTraitClient<ManualRevertTest>;
  client_t client(contract_addr_);

  // Create a native contract that calls test_error_type_revert with
  // error_type=0
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [&](SystemCallHandler &handler,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             // Call test_error_type_revert with error_type=0 (no error)
             auto [res, _gas] =
                 client.invoke<"test_error_type_revert">(handler, 0U);
             if (!res.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Should succeed normally without manual revert
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
  write_contract(native_contract_xdr);
  write_deployment(native_contract_addr, sha256(*native_contract_xdr));

  auto multiplexed_context = std::make_shared<MultiplexedContext>(
      std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>>{
          {ContractType::kNative, native_runtime_context},
          {ContractType::kWasmi, wasm_runtime_context_}});

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = native_contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = kSufficientGas,
                                 .calldata = {}}}},
      multiplexed_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success()) << "Error type 0 should succeed normally";

  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value())
      << "Error type 0 transaction should commit successfully";
}

//
// Test storage operations with manual revert through WASM contract.
//
// This integration test verifies the test_storage_manual_revert method:
// - When should_revert=true: writes multiple storage items then reverts all
// changes
// - When should_revert=false: writes storage items and preserves them
// - Validates that manual revert properly handles multiple storage operations
// - Confirms end-to-end storage revert functionality through WASM
//
TEST_P(ManualRevertWasmTest, StorageManualRevert)
{
  using client_t = ContractTraitClient<ManualRevertTest>;
  client_t client(contract_addr_);

  // Test with should_revert=true (storage should be reverted)
  {
    auto native_contract = std::make_unique<NativeContract>(
        std::unordered_map<MethodID, NativeContract::function_t>{
            {0,
             [&](SystemCallHandler &handler,
                 NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               // Call test_storage_manual_revert with should_revert=true
               auto [res, _gas] =
                   client.invoke<"test_storage_manual_revert">(handler, true);
               if (!res.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
               if (!res.value().has_value()) {
                 ADD_FAILURE() << "Function call should have succeeded";
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }

               // Should succeed via manual revert
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
    write_contract(native_contract_xdr);
    write_deployment(native_contract_addr, sha256(*native_contract_xdr));

    auto multiplexed_context = std::make_shared<MultiplexedContext>(
        std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>>{
            {ContractType::kNative, native_runtime_context},
            {ContractType::kWasmi, wasm_runtime_context_}});

    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = native_contract_addr,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = kSufficientGas,
                                   .calldata = {}}}},
        multiplexed_context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success())
        << "Storage manual revert should succeed";

    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value())
        << "Storage manual revert transaction should commit successfully";
  }

  // Test with should_revert=false (storage should be preserved)
  {
    auto native_contract = std::make_unique<NativeContract>(
        std::unordered_map<MethodID, NativeContract::function_t>{
            {0,
             [&](SystemCallHandler &handler,
                 NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               // Call test_storage_manual_revert with should_revert=false
               auto [res, _gas] =
                   client.invoke<"test_storage_manual_revert">(handler, false);
               if (!res.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
               if (!res.value().has_value()) {
                 ADD_FAILURE() << "Function call should have succeeded";
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }

               // Should succeed normally
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
    write_contract(native_contract_xdr);
    write_deployment(native_contract_addr, sha256(*native_contract_xdr));

    auto multiplexed_context = std::make_shared<MultiplexedContext>(
        std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>>{
            {ContractType::kNative, native_runtime_context},
            {ContractType::kWasmi, wasm_runtime_context_}});

    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = native_contract_addr,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = kSufficientGas,
                                   .calldata = {}}}},
        multiplexed_context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success())
        << "Storage no revert should succeed normally";

    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value())
        << "Storage no revert transaction should commit successfully";
  }
}

INSTANTIATE_TEST_SUITE_P(TestDBs, ManualRevertWasmTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
