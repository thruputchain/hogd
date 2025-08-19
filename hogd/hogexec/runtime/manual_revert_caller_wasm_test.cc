#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "rpc_types.hh"
#include "test_contracts/manual_revert_caller.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

//
// Test fixture for manual revert caller WASM contract tests
//
class ManualRevertCallerWasmTest : public ::testing::TestWithParam<TestDB>,
                                   public NativeContractTestFramework {
public:
  ManualRevertCallerWasmTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  void SetUp() override
  {
    wasm_runtime_context_ = make_hogexec_wasm_context(RuntimeEngine::kWasmi);

    // Load and deploy the manual_revert_test contract (target)
    auto target_contract = test::load_test_contract("manual_revert_test.wasm");
    target_contract_addr_ = make_contract_address(0);
    write_contract(target_contract);
    write_deployment(target_contract_addr_, sha256(*target_contract));
    setup_grant_storage_token(target_contract_addr_, 1'000'000'000);

    // Load and deploy the manual_revert_caller contract
    auto caller_contract =
        test::load_test_contract("manual_revert_caller.wasm");
    caller_contract_addr_ = make_contract_address(1);
    write_contract(caller_contract);
    write_deployment(caller_contract_addr_, sha256(*caller_contract));
    setup_grant_storage_token(caller_contract_addr_, 1'000'000'000);
  }

  std::shared_ptr<RuntimeContext> wasm_runtime_context_;
  ContractAddress target_contract_addr_;
  ContractAddress caller_contract_addr_;
};
// NOLINTBEGIN(bugprone-unchecked-optional-access)

//
// Test that basic cross-contract revert functionality works
//
TEST_P(ManualRevertCallerWasmTest, BasicCrossContractRevert)
{
  // Use ContractTraitClient to call the manual_revert_caller contract
  using caller_client_t =
      ContractTraitClient<manual_revert_caller::ManualRevertCaller>;
  caller_client_t caller_client(caller_contract_addr_);

  // Initialize the manual_revert_caller contract with the manual_revert_test
  // contract's address
  auto init_handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                caller_client.make_invocation<"initialize">(
                                    UINT64_MAX, target_contract_addr_)}},
      wasm_runtime_context_);

  auto init_res = init_handler.root_invoke();
  ASSERT_TRUE(init_res.is_root_success())
      << "Failed to initialize manual_revert_caller contract";

  auto init_commit_res = init_handler.try_commit_transaction();
  ASSERT_TRUE(init_commit_res.has_value()) << "Failed to commit initialization";
  ASSERT_TRUE(*init_commit_res);

  advance_block();

  // Call the manual_revert_caller contract's call_basic_manual_revert method
  auto call_handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation = caller_client.make_invocation<
                                "call_basic_manual_revert">(UINT64_MAX, true)}},
      wasm_runtime_context_);

  auto call_res = call_handler.root_invoke();
  ASSERT_TRUE(call_res.is_root_success())
      << "Failed to call call_basic_manual_revert";

  auto call_commit_res = call_handler.try_commit_transaction();
  ASSERT_TRUE(call_commit_res.has_value())
      << "Failed to commit call transaction";
  ASSERT_TRUE(*call_commit_res);

  advance_block();

  // Verify that the caller's state persists while target's state is reverted
  auto caller_addrkey =
      make_contract_data_addrkey_matching_static_key_sdk_macro(
          caller_contract_addr_, "caller_state");
  auto caller_state = lookup_storage_item_from_db(caller_addrkey);

  ASSERT_TRUE(caller_state.has_value())
      << "Caller's state should persist after revert";
  ASSERT_EQ(caller_state.value().bytestring(),
            make_bytestring("caller_data_before_call"));

  // Verify that the target contract's state was reverted (should not exist)
  auto target_addrkey =
      make_contract_data_addrkey_matching_static_key_sdk_macro(
          target_contract_addr_, "test_basic_revert");
  auto target_state = lookup_storage_item_from_db(target_addrkey);
  ASSERT_FALSE(target_state.has_value())
      << "Target contract's state should be reverted";
}

// Test nested manual revert scenarios across contracts
TEST_P(ManualRevertCallerWasmTest, NestedCrossContractRevert)
{
  // Use ContractTraitClient to call the manual_revert_caller contract
  using caller_client_t =
      ContractTraitClient<manual_revert_caller::ManualRevertCaller>;
  caller_client_t caller_client(caller_contract_addr_);

  // Initialize the manual_revert_caller contract with the manual_revert_test
  // contract's address
  auto init_handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                caller_client.make_invocation<"initialize">(
                                    UINT64_MAX, target_contract_addr_)}},
      wasm_runtime_context_);

  auto init_res = init_handler.root_invoke();
  ASSERT_TRUE(init_res.is_root_success())
      << "Failed to initialize manual_revert_caller contract";

  auto init_commit_res = init_handler.try_commit_transaction();
  ASSERT_TRUE(init_commit_res.has_value()) << "Failed to commit initialization";
  ASSERT_TRUE(*init_commit_res);

  advance_block();

  // Call the manual_revert_caller contract's call_nested_manual_revert method
  auto call_handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{.invocation = caller_client.make_invocation<
                              "call_nested_manual_revert">(UINT64_MAX, true)}},
      wasm_runtime_context_);

  auto call_res = call_handler.root_invoke();
  ASSERT_TRUE(call_res.is_root_success())
      << "Failed to call call_nested_manual_revert";

  auto call_commit_res = call_handler.try_commit_transaction();
  ASSERT_TRUE(call_commit_res.has_value())
      << "Failed to commit call transaction";
  ASSERT_TRUE(*call_commit_res);

  advance_block();

  // Verify that the caller's state persists while target's state is reverted
  auto caller_addrkey =
      make_contract_data_addrkey_matching_static_key_sdk_macro(
          caller_contract_addr_, "caller_nested_state");
  auto caller_state = lookup_storage_item_from_db(caller_addrkey);

  ASSERT_TRUE(caller_state.has_value())
      << "Caller's nested state should persist after revert";
  ASSERT_EQ(caller_state.value().bytestring(),
            make_bytestring("caller_nested_data"));

  // Verify that the target contract's state was reverted (should not exist)
  auto target_addrkey =
      make_contract_data_addrkey_matching_static_key_sdk_macro(
          target_contract_addr_, "test_nested_revert");
  auto target_state = lookup_storage_item_from_db(target_addrkey);
  ASSERT_FALSE(target_state.has_value())
      << "Target contract's nested state should be reverted";
}

// Test error type revert scenarios across contracts
TEST_P(ManualRevertCallerWasmTest, ErrorTypeCrossContractRevert)
{
  // Use ContractTraitClient to call the manual_revert_caller contract
  using caller_client_t =
      ContractTraitClient<manual_revert_caller::ManualRevertCaller>;
  caller_client_t caller_client(caller_contract_addr_);

  // Initialize the manual_revert_caller contract with the manual_revert_test
  // contract's address
  auto init_handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                caller_client.make_invocation<"initialize">(
                                    UINT64_MAX, target_contract_addr_)}},
      wasm_runtime_context_);

  auto init_res = init_handler.root_invoke();
  ASSERT_TRUE(init_res.is_root_success())
      << "Failed to initialize manual_revert_caller contract";

  auto init_commit_res = init_handler.try_commit_transaction();
  ASSERT_TRUE(init_commit_res.has_value()) << "Failed to commit initialization";
  ASSERT_TRUE(*init_commit_res);

  advance_block();

  // Call the manual_revert_caller contract's call_error_type_revert method
  auto call_handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      caller_client.make_invocation<"call_error_type_revert">(
                          UINT64_MAX, uint32_t{1})}},
      wasm_runtime_context_);

  auto call_res = call_handler.root_invoke();
  ASSERT_TRUE(call_res.is_root_success())
      << "Failed to call call_error_type_revert";

  auto call_commit_res = call_handler.try_commit_transaction();
  ASSERT_TRUE(call_commit_res.has_value())
      << "Failed to commit call transaction";
  ASSERT_TRUE(*call_commit_res);

  advance_block();

  // Verify that the caller's state persists while target's state is reverted
  auto caller_addrkey =
      make_contract_data_addrkey_matching_static_key_sdk_macro(
          caller_contract_addr_, "caller_error_state");
  auto caller_state = lookup_storage_item_from_db(caller_addrkey);

  ASSERT_TRUE(caller_state.has_value())
      << "Caller's error state should persist after revert";
  ASSERT_EQ(caller_state.value().bytestring(),
            make_bytestring("caller_error_data"));

  // Verify that the target contract's state was reverted (should not exist)
  auto target_addrkey =
      make_contract_data_addrkey_matching_static_key_sdk_macro(
          target_contract_addr_, "test_error_type_revert");
  auto target_state = lookup_storage_item_from_db(target_addrkey);
  ASSERT_FALSE(target_state.has_value())
      << "Target contract's error state should be reverted";
}

// Test storage manual revert scenarios across contracts
TEST_P(ManualRevertCallerWasmTest, StorageCrossContractRevert)
{
  // Use ContractTraitClient to call the manual_revert_caller contract
  using caller_client_t =
      ContractTraitClient<manual_revert_caller::ManualRevertCaller>;
  caller_client_t caller_client(caller_contract_addr_);

  // Initialize the manual_revert_caller contract with the manual_revert_test
  // contract's address
  auto init_handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                caller_client.make_invocation<"initialize">(
                                    UINT64_MAX, target_contract_addr_)}},
      wasm_runtime_context_);

  auto init_res = init_handler.root_invoke();
  ASSERT_TRUE(init_res.is_root_success())
      << "Failed to initialize manual_revert_caller contract";

  auto init_commit_res = init_handler.try_commit_transaction();
  ASSERT_TRUE(init_commit_res.has_value()) << "Failed to commit initialization";
  ASSERT_TRUE(*init_commit_res);

  advance_block();

  // Call the manual_revert_caller contract's call_storage_manual_revert method
  auto call_handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{.invocation = caller_client.make_invocation<
                              "call_storage_manual_revert">(UINT64_MAX, true)}},
      wasm_runtime_context_);

  auto call_res = call_handler.root_invoke();
  ASSERT_TRUE(call_res.is_root_success())
      << "Failed to call call_storage_manual_revert";

  auto call_commit_res = call_handler.try_commit_transaction();
  ASSERT_TRUE(call_commit_res.has_value())
      << "Failed to commit call transaction";
  ASSERT_TRUE(*call_commit_res);

  advance_block();

  // Verify that the caller's state persists while target's state is reverted
  auto caller_addrkey =
      make_contract_data_addrkey_matching_static_key_sdk_macro(
          caller_contract_addr_, "caller_storage_state");
  auto caller_state = lookup_storage_item_from_db(caller_addrkey);

  ASSERT_TRUE(caller_state.has_value())
      << "Caller's storage state should persist after revert";
  ASSERT_EQ(caller_state.value().bytestring(),
            make_bytestring("caller_storage_data"));

  // Verify that the target contract's state was reverted (should not exist)
  auto target_addrkey =
      make_contract_data_addrkey_matching_static_key_sdk_macro(
          target_contract_addr_, "test_storage_revert");
  auto target_state = lookup_storage_item_from_db(target_addrkey);
  ASSERT_FALSE(target_state.has_value())
      << "Target contract's storage state should be reverted";
}

// Test that caller's state persists and can be checked
TEST_P(ManualRevertCallerWasmTest, StatePersistenceAndCheck)
{
  // Use ContractTraitClient to call the manual_revert_caller contract
  using caller_client_t =
      ContractTraitClient<manual_revert_caller::ManualRevertCaller>;
  caller_client_t caller_client(caller_contract_addr_);

  // Initialize the manual_revert_caller contract with the manual_revert_test
  // contract's address
  auto init_handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                caller_client.make_invocation<"initialize">(
                                    UINT64_MAX, target_contract_addr_)}},
      wasm_runtime_context_);

  auto init_res = init_handler.root_invoke();
  ASSERT_TRUE(init_res.is_root_success())
      << "Failed to initialize manual_revert_caller contract";

  auto init_commit_res = init_handler.try_commit_transaction();
  ASSERT_TRUE(init_commit_res.has_value()) << "Failed to commit initialization";
  ASSERT_TRUE(*init_commit_res);

  advance_block();

  // Write some state to the caller contract
  std::string_view test_str = "test_persistent_data";
  x23::Vec<uint8_t, x23::kMaxXdrSize> test_data(
      reinterpret_cast<const uint8_t *>(test_str.data()),
      reinterpret_cast<const uint8_t *>(test_str.data() + test_str.size()));
  auto write_handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{.invocation =
                              caller_client.make_invocation<"write_own_state">(
                                  UINT64_MAX, test_data)}},
      wasm_runtime_context_);

  auto write_res = write_handler.root_invoke();
  ASSERT_TRUE(write_res.is_root_success()) << "Failed to write own state";

  auto write_commit_res = write_handler.try_commit_transaction();
  ASSERT_TRUE(write_commit_res.has_value())
      << "Failed to commit write transaction";
  ASSERT_TRUE(*write_commit_res);

  advance_block();

  // Check that the state exists using the contract's method
  auto check_handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      caller_client.make_invocation<"check_own_state_exists">(
                          UINT64_MAX)}},
      wasm_runtime_context_);

  auto check_res = check_handler.root_invoke();
  ASSERT_TRUE(check_res.is_root_success())
      << "Failed to check own state exists";

  auto check_commit_res = check_handler.try_commit_transaction();
  ASSERT_TRUE(check_commit_res.has_value())
      << "Failed to commit check transaction";
  ASSERT_TRUE(*check_commit_res);

  advance_block();

  // Verify the state was written correctly
  auto caller_addrkey =
      make_contract_data_addrkey_matching_static_key_sdk_macro(
          caller_contract_addr_, "caller_persistent_state");
  auto caller_state = lookup_storage_item_from_db(caller_addrkey);

  ASSERT_TRUE(caller_state.has_value())
      << "Caller's persistent state should exist";
  ASSERT_EQ(caller_state.value().bytestring(),
            make_bytestring("test_persistent_data"));
}

// Check that the native contract test framework's implementation of static_key
// matches the sdk's version
TEST_P(ManualRevertCallerWasmTest, StaticKeyMacro)
{
  auto native_key =
      make_storage_key_matching_static_key_sdk_macro("caller_state");
  // 5419c6683809412807d947208ed482ad0fe5016ab44433c0217470e07ab0a116
  auto sdk_key =
      std::array<uint8_t, 32>{0x54, 0x19, 0xc6, 0x68, 0x38, 0x09, 0x41, 0x28,
                              0x07, 0xd9, 0x47, 0x20, 0x8e, 0xd4, 0x82, 0xad,
                              0x0f, 0xe5, 0x01, 0x6a, 0xb4, 0x44, 0x33, 0xc0,
                              0x21, 0x74, 0x70, 0xe0, 0x7a, 0xb0, 0xa1, 0x16};
  ASSERT_EQ(sdk_key, native_key);
}

INSTANTIATE_TEST_SUITE_P(TestDBs, ManualRevertCallerWasmTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
