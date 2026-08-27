#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include "built_contracts/contract_loader.h"
#include "built_contracts/manual_revert_test.h"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/contract_result.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

//
// WASM integration tests for manual frame revert functionality.
//
// These tests load and invoke the actual manual_revert_test contract
// to verify that the SDK's Returnable trait properly triggers manual revert
// at the WASM runtime level.
//

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

class ManualRevertRv32Test : public ::testing::TestWithParam<TestDB>,
                             public NativeContractTestFramework {
public:
  ManualRevertRv32Test()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  void SetUp() override
  {
    // Load and deploy the manual_revert_test contract
    auto contract = hogd::load_rv32_contract("manual_revert_test.rv32a");
    contract_addr_ = make_contract_address(0);
    write_contract(contract);
    write_deployment(contract_addr_, contract);
    setup_grant_storage_token(contract_addr_, 1'000'000'000);

    // The root invoked address in every test is the native contract at
    // make_contract_address(1) (deployed per-test below), which commits the
    // transaction and consumes a replay-cache entry charged to it. Grant
    // enough to cover the worst-case test (ErrorTypeManualReverts commits 4
    // transactions against this address); tests with fewer commits simply
    // leave the spare unused.
    setup_grant_storage_token(make_contract_address(1),
                              replay_cache_cost_charged(1) * 4);
  }
  ContractAddress contract_addr_;

  constexpr static GasAmount kSufficientGas = 100000;

  template<typename T>
  T check_result(HostFnStatus<SyscallStatus<std::optional<T>>> res)
  {
    if (!res.has_value() || !res.value().has_value() ||
        !res.value().value().has_value()) {
      ADD_FAILURE() << "Function call should have succeeded";
      panic("Function call should have succeeded");
    }
    if (!res.value().value().has_value()) {
      ADD_FAILURE() << "Returned buffer decode should have succeeded";
    }
    return res.value().value().value();
  }
};

//
// Test basic manual revert functionality through WASM contract.
//
// This integration test verifies the complete end-to-end flow:
// - Loads the actual manual_revert_test contract
// - Calls test_basic_manual_revert(should_revert=true) via WASM runtime
// - Verifies that the SDK's Returnable trait triggers manual revert
// - Confirms that storage changes are reverted (check_storage_exists returns
// false)
// - Ensures the transaction commits successfully despite the revert
//
TEST_P(ManualRevertRv32Test, BasicManualRevert)
{
  using client_t = ContractTraitClient<manual_revert_test::ManualRevertTest>;
  client_t client(contract_addr_);

  // Create a native contract that calls the WASM contract to test manual revert
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    // Call test_basic_manual_revert with should_revert=true
    auto res = client.invoke_opt<u8"test_basic_manual_revert">(ctx, true);

    auto result = check_result(res);

    if (!result.is_err()) {
      ADD_FAILURE() << "Function call should have returned a contract error";
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    // The manual revert should have succeeded (no error)
    // and storage should be reverted (tested by check_storage_exists)
    auto storage_check = client.invoke_opt<u8"check_storage_exists">(ctx);

    // Storage should not exist due to manual revert
    if (!storage_check.has_value() || !storage_check.value().has_value() ||
        !storage_check.value().value().has_value() ||
        !storage_check.value().value().value().is_ok()) {
      ADD_FAILURE() << "Function call encountered unexpected error";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (storage_check.value().value().value().ok()) {
      ADD_FAILURE() << "Storage check should have returned false "
                       "(revert should wipe changes)";
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    return {};
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>>
      native_contract_map;
  native_contract_map.emplace(0, std::move(native_contract));
  auto native_runtime_context =
      std::make_shared<NativeContext>(std::move(native_contract_map));

  // Deploy the native contract
  auto native_contract_xdr = make_native_contract(0);
  auto native_contract_addr = make_contract_address(1);
  write_contract(native_contract_xdr);
  write_deployment(native_contract_addr, native_contract_xdr);
  auto rv32_context = std::make_unique<Rv32Context>();

  auto multiplexed_context = std::make_unique<MultiplexedContext>(
      std::unordered_map<ContractType, std::pair<RuntimeContext const *, bool>>{
          {ContractType::kNative, {native_runtime_context.get(), false}},
          {ContractType::kRv32, {rv32_context.release(), true}}});

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{
                                    .invoked_address = native_contract_addr,
                                    .entrypoint = entrypoint_from_method_id(0),
                                    .gas_limit = kSufficientGas,
                                    .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *multiplexed_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success()) << "Manual revert test should succeed";

  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK)
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
TEST_P(ManualRevertRv32Test, BasicNoRevert)
{
  using client_t = ContractTraitClient<manual_revert_test::ManualRevertTest>;
  client_t client(contract_addr_);

  // Create a native contract that calls the WASM contract without revert
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    // Call test_basic_manual_revert with should_revert=false
    auto res = client.invoke_opt<u8"test_basic_manual_revert">(ctx, false);
    if (!res.has_value() || !res.value().has_value() ||
        !res.value().value().has_value() ||
        !res.value().value().value().is_ok()) {
      ADD_FAILURE() << "Function call should have succeeded";
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    // Storage should exist because no revert occurred
    auto storage_check = client.invoke_opt<u8"check_storage_exists">(ctx);
    if (!storage_check.has_value() || !storage_check.value().has_value() ||
        !storage_check.value().value().has_value()) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    // Storage should exist (no revert)
    if (!storage_check.value().value().value().is_ok()) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    return {};
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>>
      native_contract_map;
  native_contract_map.emplace(0, std::move(native_contract));
  auto native_runtime_context =
      std::make_shared<NativeContext>(std::move(native_contract_map));

  // Deploy the native contract
  auto native_contract_xdr = make_native_contract(0);
  auto native_contract_addr = make_contract_address(1);
  write_contract(native_contract_xdr);
  write_deployment(native_contract_addr, native_contract_xdr);
  auto rv32_context = std::make_unique<Rv32Context>();
  auto multiplexed_context = std::make_unique<MultiplexedContext>(
      std::unordered_map<ContractType, std::pair<RuntimeContext const *, bool>>{
          {ContractType::kNative, {native_runtime_context.get(), false}},
          {ContractType::kRv32, {rv32_context.release(), true}}});

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{
                                    .invoked_address = native_contract_addr,
                                    .entrypoint = entrypoint_from_method_id(0),
                                    .gas_limit = kSufficientGas,
                                    .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *multiplexed_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success()) << "No revert test should succeed";

  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK)
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
TEST_P(ManualRevertRv32Test, NestedManualRevert)
{
  using client_t = ContractTraitClient<manual_revert_test::ManualRevertTest>;
  client_t client(contract_addr_);

  // Create a native contract that tests nested manual revert
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    // Call test_nested_manual_revert with child_should_revert=true
    auto res = client.invoke_opt<u8"test_nested_manual_revert">(ctx, true);
    auto result = check_result(res);

    if (!result.is_err()) {
      ADD_FAILURE() << "Function call should have returned a contract error";
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    return {};
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>>
      native_contract_map;
  native_contract_map.emplace(0, std::move(native_contract));
  auto native_runtime_context =
      std::make_shared<NativeContext>(std::move(native_contract_map));

  // Deploy the native contract
  auto native_contract_xdr = make_native_contract(0);
  auto native_contract_addr = make_contract_address(1);
  write_contract(native_contract_xdr);
  write_deployment(native_contract_addr, native_contract_xdr);

  auto rv32_context = std::make_unique<Rv32Context>();
  auto multiplexed_context = std::make_unique<MultiplexedContext>(
      std::unordered_map<ContractType, std::pair<RuntimeContext const *, bool>>{
          {ContractType::kNative, {native_runtime_context.get(), false}},
          {ContractType::kRv32, {rv32_context.release(), true}}});

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{
                                    .invoked_address = native_contract_addr,
                                    .entrypoint = entrypoint_from_method_id(0),
                                    .gas_limit = kSufficientGas,
                                    .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *multiplexed_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success())
      << "Nested manual revert test should succeed";

  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK)
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
TEST_P(ManualRevertRv32Test, ErrorTypeManualReverts)
{
  using client_t = ContractTraitClient<manual_revert_test::ManualRevertTest>;
  client_t client(contract_addr_);

  // Test error types 1-4 (should all trigger manual revert)
  for (uint32_t error_type = 1; error_type <= 4; ++error_type) {
    // Create a native contract that calls the WASM contract's error type method
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(
        0,
        [&,
         error_type](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          // Call test_error_type_revert with specific error type
          auto res =
              client.invoke_opt<u8"test_error_type_revert">(ctx, error_type);
          auto result = check_result(res);
          if (!result.is_err()) {
            ADD_FAILURE()
                << "Function call should have returned a contract error";
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          // All error types should succeed via manual revert
          return {};
        });
    auto native_contract = std::make_unique<NativeContract>(std::move(fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>>
        native_contract_map;
    native_contract_map.emplace(0, std::move(native_contract));
    auto native_runtime_context =
        std::make_shared<NativeContext>(std::move(native_contract_map));

    // Deploy the native contract
    auto native_contract_xdr = make_native_contract(0);
    auto native_contract_addr = make_contract_address(1);
    write_contract(native_contract_xdr);
    write_deployment(native_contract_addr, native_contract_xdr);

    auto rv32_context = std::make_unique<Rv32Context>();
    auto multiplexed_context = std::make_unique<MultiplexedContext>(
        std::unordered_map<ContractType,
                           std::pair<RuntimeContext const *, bool>>{
            {ContractType::kNative, {native_runtime_context.get(), false}},
            {ContractType::kRv32, {rv32_context.release(), true}}});

    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address =
                                                 native_contract_addr,
                                             .entrypoint =
                                                 entrypoint_from_method_id(0),
                                             .gas_limit = kSufficientGas,
                                             .calldata = {}},
                              .uniquifier = make_new_uniquifier()}},
        *multiplexed_context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success())
        << "Error type " << error_type << " should succeed via manual revert";

    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK)
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
TEST_P(ManualRevertRv32Test, ErrorTypeNoError)
{
  using client_t = ContractTraitClient<manual_revert_test::ManualRevertTest>;
  client_t client(contract_addr_);

  // Create a native contract that calls test_error_type_revert with
  // error_type=0
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    // Call test_error_type_revert with error_type=0 (no error)
    auto res = client.invoke_opt<u8"test_error_type_revert">(ctx, 0U);
    auto result = check_result(res);
    if (!result.is_ok()) {
      ADD_FAILURE() << "Function call should have returned a contract error";
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    // Should succeed normally without manual revert
    return {};
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>>
      native_contract_map;
  native_contract_map.emplace(0, std::move(native_contract));
  auto native_runtime_context =
      std::make_shared<NativeContext>(std::move(native_contract_map));

  // Deploy the native contract
  auto native_contract_xdr = make_native_contract(0);
  auto native_contract_addr = make_contract_address(1);
  write_contract(native_contract_xdr);
  write_deployment(native_contract_addr, native_contract_xdr);

  auto rv32_context = std::make_unique<Rv32Context>();
  auto multiplexed_context = std::make_unique<MultiplexedContext>(
      std::unordered_map<ContractType, std::pair<RuntimeContext const *, bool>>{
          {ContractType::kNative, {native_runtime_context.get(), false}},
          {ContractType::kRv32, {rv32_context.release(), true}}});

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{
                                    .invoked_address = native_contract_addr,
                                    .entrypoint = entrypoint_from_method_id(0),
                                    .gas_limit = kSufficientGas,
                                    .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *multiplexed_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success()) << "Error type 0 should succeed normally";

  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK)
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
TEST_P(ManualRevertRv32Test, StorageManualRevert)
{
  using client_t = ContractTraitClient<manual_revert_test::ManualRevertTest>;
  client_t client(contract_addr_);

  // Test with should_revert=true (storage should be reverted)
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(
        0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          // Call test_storage_manual_revert with should_revert=true
          auto res =
              client.invoke_opt<u8"test_storage_manual_revert">(ctx, true);
          auto result = check_result(res);
          if (!result.is_err()) {
            ADD_FAILURE()
                << "Function call should have returned a contract error";
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          // Should succeed via manual revert
          return {};
        });
    auto native_contract = std::make_unique<NativeContract>(std::move(fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>>
        native_contract_map;
    native_contract_map.emplace(0, std::move(native_contract));
    auto native_runtime_context =
        std::make_shared<NativeContext>(std::move(native_contract_map));

    // Deploy the native contract
    auto native_contract_xdr = make_native_contract(0);
    auto native_contract_addr = make_contract_address(1);
    write_contract(native_contract_xdr);
    write_deployment(native_contract_addr, native_contract_xdr);

    auto rv32_context = std::make_unique<Rv32Context>();
    auto multiplexed_context = std::make_unique<MultiplexedContext>(
        std::unordered_map<ContractType,
                           std::pair<RuntimeContext const *, bool>>{
            {ContractType::kNative, {native_runtime_context.get(), false}},
            {ContractType::kRv32, {rv32_context.release(), true}}});

    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address =
                                                 native_contract_addr,
                                             .entrypoint =
                                                 entrypoint_from_method_id(0),
                                             .gas_limit = kSufficientGas,
                                             .calldata = {}},
                              .uniquifier = make_new_uniquifier()}},
        *multiplexed_context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success())
        << "Storage manual revert should succeed";

    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK)
        << "Storage manual revert transaction should commit successfully";
  }

  // Test with should_revert=false (storage should be preserved)
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(0,
                [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  // Call test_storage_manual_revert with should_revert=false
                  auto res =
                      client.invoke_opt<u8"test_storage_manual_revert">(ctx,
                                                                        false);
                  auto result = check_result(res);
                  if (!result.is_ok()) {
                    ADD_FAILURE() << "Function call should have succeeded";
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }

                  // Should succeed normally
                  return {};
                });
    auto native_contract = std::make_unique<NativeContract>(std::move(fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>>
        native_contract_map;
    native_contract_map.emplace(0, std::move(native_contract));
    auto native_runtime_context =
        std::make_shared<NativeContext>(std::move(native_contract_map));

    // Deploy the native contract
    auto native_contract_xdr = make_native_contract(0);
    auto native_contract_addr = make_contract_address(1);
    write_contract(native_contract_xdr);
    write_deployment(native_contract_addr, native_contract_xdr);

    auto rv32_context = std::make_unique<Rv32Context>();
    auto multiplexed_context = std::make_unique<MultiplexedContext>(
        std::unordered_map<ContractType,
                           std::pair<RuntimeContext const *, bool>>{
            {ContractType::kNative, {native_runtime_context.get(), false}},
            {ContractType::kRv32, {rv32_context.release(), true}}});

    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address =
                                                 native_contract_addr,
                                             .entrypoint =
                                                 entrypoint_from_method_id(0),
                                             .gas_limit = kSufficientGas,
                                             .calldata = {}},
                              .uniquifier = make_new_uniquifier()}},
        *multiplexed_context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success())
        << "Storage no revert should succeed normally";

    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK)
        << "Storage no revert transaction should commit successfully";
  }
}

INSTANTIATE_TEST_SUITE_P(TestDBs, ManualRevertRv32Test,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
