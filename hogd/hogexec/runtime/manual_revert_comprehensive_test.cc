#include <gtest/gtest.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/db_interface/block_manager.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "hogd/mockdb/in_memory_database.h"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class ManualRevertTest : public ::testing::Test,
                         public NativeContractTestFramework {
public:
  ManualRevertTest() : NativeContractTestFramework(false) {}
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// Test the key new feature: manual revert with return data
// State should be reverted but transaction should commit successfully
TEST_F(ManualRevertTest, ManualRevertBasicFunctionality)
{
  ContractAddress contract_addr = make_contract_address(1);
  auto str_hello = make_bytestring("hello");
  auto return_data = make_bytestring("error_occurred");
  auto key = make_storage_key("test_key");

  // Contract that writes state, then reverts with return data
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [str_hello, return_data,
            key](SystemCallHandler &handler,
                 NativeRuntime &) -> InvokeStatus<void> {
             // Write state that should be reverted
             auto res =
                 handler.set_bytestring(key,
                                        std::as_bytes(std::span(str_hello)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Manually revert with return data
             ReturnBuffer buffer;
             buffer.resize_uninitialized(return_data.size());
             std::memcpy(buffer.data(), return_data.data(), return_data.size());
             handler.set_return_buffer(std::move(buffer), true);
             return {};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contract
  auto contract = make_native_contract(0);
  in_memory_db_.write_contract(sha256(*contract), contract);
  in_memory_db_.write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Invoke the contract
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  // Transaction should still commit successfully (this is the key difference
  // from normal revert)
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);

  advance_block();

  // State should be reverted (bytestring not written)
  auto data_addrkey = make_contract_data_addrkey(contract_addr, "test_key");
  auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
  ASSERT_FALSE(data_item_opt.has_value());
}

// Test nested manual revert: child reverts with data, parent gets the data
TEST_F(ManualRevertTest, NestedManualRevertWithReturnData)
{
  ContractAddress contract_addr = make_contract_address(1);
  auto parent_data = make_bytestring("parent_state");
  auto child_data = make_bytestring("child_state");
  auto child_return_data = make_bytestring("child_error");
  auto parent_key = make_storage_key("parent_key");
  auto child_key = make_storage_key("child_key");

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          // Parent method (0)
          {0,
           [parent_data,
            parent_key](SystemCallHandler &handler,
                        NativeRuntime &runtime) -> InvokeStatus<void> {
             // Parent writes its own state
             auto res =
                 handler.set_bytestring(parent_key,
                                        std::as_bytes(std::span(parent_data)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Invoke child method that will revert
             auto [r1, _g1] =
                 handler.invoke(handler.get_current_address(), 1,
                                runtime.get_available_gas(), {}, 0);
             if (!r1.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (r1.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Check that child's return data is available
             auto status = handler.get_return_buffer();
             if (!status) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Parent should be able to access child's return data
             auto expected_child_data = make_bytestring("child_error");
             if ((*status)->size() != expected_child_data.size()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (std::memcmp((*status)->data(), expected_child_data.data(),
                             expected_child_data.size()) != 0) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }},
          // Child method (1) - manually reverts with return data
          {1,
           [child_data, child_return_data,
            child_key](SystemCallHandler &handler,
                       NativeRuntime &) -> InvokeStatus<void> {
             // Child writes state that should be reverted
             auto res =
                 handler.set_bytestring(child_key,
                                        std::as_bytes(std::span(child_data)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Child manually reverts with return data
             ReturnBuffer buffer;
             buffer.resize_uninitialized(child_return_data.size());
             std::memcpy(buffer.data(), child_return_data.data(),
                         child_return_data.size());
             handler.set_return_buffer(std::move(buffer), true);
             return {};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contract
  auto contract = make_native_contract(0);
  in_memory_db_.write_contract(sha256(*contract), contract);
  in_memory_db_.write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Invoke parent method
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);

  advance_block();

  // Parent state should persist (not reverted)
  auto parent_addrkey = make_contract_data_addrkey(contract_addr, "parent_key");
  auto parent_item_opt = lookup_storage_item_from_db(parent_addrkey);
  ASSERT_TRUE(parent_item_opt.has_value());

  // Child state should be reverted (not written)
  auto child_addrkey = make_contract_data_addrkey(contract_addr, "child_key");
  auto child_item_opt = lookup_storage_item_from_db(child_addrkey);
  ASSERT_FALSE(child_item_opt.has_value());
}

// Test difference between manual revert and normal failure
TEST_F(ManualRevertTest, ManualRevertVsNormalFailure)
{
  ContractAddress contract_addr = make_contract_address(1);
  auto test_data = make_bytestring("test_state");
  auto return_data = make_bytestring("manual_revert_data");
  auto key = make_storage_key("test_key");

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          // Method 0: Manual revert with return data
          {0,
           [test_data, return_data,
            key](SystemCallHandler &handler,
                 NativeRuntime &) -> InvokeStatus<void> {
             auto res =
                 handler.set_bytestring(key,
                                        std::as_bytes(std::span(test_data)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             ReturnBuffer buffer;
             buffer.resize_uninitialized(return_data.size());
             std::memcpy(buffer.data(), return_data.data(), return_data.size());
             handler.set_return_buffer(std::move(buffer), true);
             return {};
           }},
          // Method 1: Normal failure
          {1,
           [test_data, key](SystemCallHandler &handler,
                            NativeRuntime &) -> InvokeStatus<void> {
             auto res =
                 handler.set_bytestring(key,
                                        std::as_bytes(std::span(test_data)));
             if (!res.has_value()) {
               ADD_FAILURE() << "set_bytestring failed";
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               ADD_FAILURE() << "set_bytestring failed";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Normal failure - no return data
             return InvokeError{InvokeErrorCode::kInvokeDeterministic};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contract
  auto contract = make_native_contract(0);
  in_memory_db_.write_contract(sha256(*contract), contract);
  in_memory_db_.write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Test manual revert (method 0) - should succeed
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address = contract_addr,
                                             .entrypoint =
                                                 entrypoint_from_method_id(0),
                                             .gas_limit = UINT64_MAX,
                                             .calldata = {}}}},
        runtime_context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success()); // Manual revert should succeed

    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res); // Should commit successfully

    advance_block();
  }

  // Test normal failure (method 1) - should fail
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address = contract_addr,
                                             .entrypoint =
                                                 entrypoint_from_method_id(1),
                                             .gas_limit = UINT64_MAX,
                                             .calldata = {}}}},
        runtime_context);

    auto res = handler.root_invoke();
    ASSERT_FALSE(res.is_root_success()); // Normal failure should fail

    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_FALSE(*commit_res); // Should not commit

    advance_block();
  }

  // In both cases, state should not persist
  auto data_addrkey = make_contract_data_addrkey(contract_addr, "test_key");
  auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
  ASSERT_FALSE(data_item_opt.has_value());
}

// Test gas consumption with manual revert
TEST_F(ManualRevertTest, GasConsumptionWithManualRevert)
{
  ContractAddress contract_addr = make_contract_address(1);
  auto test_data = make_bytestring("gas_test");
  auto return_data = make_bytestring("revert_data");

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [test_data,
            return_data](SystemCallHandler &handler,
                         NativeRuntime &runtime) -> InvokeStatus<void> {
             // Do expensive work before reverting
             for (int i = 0; i < 10; ++i) {
               auto temp_key = make_storage_key("temp_" + std::to_string(i));
               auto res =
                   handler.set_bytestring(temp_key,
                                          std::as_bytes(std::span(test_data)));
               if (!res.has_value()) {
                 return res.error().to_invoke_error();
               }
               if (!res.value().has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }

               if (!runtime.consume_gas(1000)) {
                 ADD_FAILURE() << "Failed to consume gas";
               }
             }

             // Manual revert with return data
             ReturnBuffer buffer;
             buffer.resize_uninitialized(return_data.size());
             std::memcpy(buffer.data(), return_data.data(), return_data.size());
             handler.set_return_buffer(std::move(buffer), true);
             return {};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contract
  auto contract = make_native_contract(0);
  in_memory_db_.write_contract(sha256(*contract), contract);
  in_memory_db_.write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Invoke with limited gas
  const uint64_t gas_limit = 1'000'000;
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = gas_limit,
                                           .calldata = {}}}},
      runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  // Gas consumption behavior (native contracts may or may not consume gas in
  // tests)
  auto gas_consumed = res.gas_consumed_;
  ASSERT_LT(gas_consumed, gas_limit); // Should not exceed limit
  ASSERT_GE(gas_consumed, 10000);

  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);

  advance_block();

  // All state should be reverted
  for (int i = 0; i < 10; ++i) {
    auto temp_addrkey =
        make_contract_data_addrkey(contract_addr, "temp_" + std::to_string(i));
    auto temp_item_opt = lookup_storage_item_from_db(temp_addrkey);
    ASSERT_FALSE(temp_item_opt.has_value());
  }
}

// Test multiple manual reverts in sequence (nested calls)
TEST_F(ManualRevertTest, MultipleNestedManualReverts)
{
  ContractAddress contract_addr = make_contract_address(1);
  auto data1 = make_bytestring("level1");
  auto data2 = make_bytestring("level2");
  auto data3 = make_bytestring("level3");
  auto return1 = make_bytestring("return1");
  auto return2 = make_bytestring("return2");
  auto return3 = make_bytestring("return3");

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          // Level 1 - calls level 2, gets reverted data, then reverts itself
          {1,
           [data1, return1](SystemCallHandler &handler,
                            NativeRuntime &runtime) -> InvokeStatus<void> {
             auto key = make_storage_key("level1");
             auto res =
                 handler.set_bytestring(key, std::as_bytes(std::span(data1)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Call level 2
             auto [r2, _g2] =
                 handler.invoke(handler.get_current_address(), 2,
                                runtime.get_available_gas(), {}, 0);
             if (!r2.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (r2.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Revert with our own data
             ReturnBuffer buffer;
             buffer.resize_uninitialized(return1.size());
             std::memcpy(buffer.data(), return1.data(), return1.size());
             handler.set_return_buffer(std::move(buffer), true);
             return {};
           }},
          // Level 2 - calls level 3, gets reverted data, then reverts itself
          {2,
           [data2, return2](SystemCallHandler &handler,
                            NativeRuntime &runtime) -> InvokeStatus<void> {
             auto key = make_storage_key("level2");
             auto res =
                 handler.set_bytestring(key, std::as_bytes(std::span(data2)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Call level 3
             auto [r3, _g3] =
                 handler.invoke(handler.get_current_address(), 3,
                                runtime.get_available_gas(), {}, 0);
             if (!r3.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (r3.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Revert with our own data
             ReturnBuffer buffer;
             buffer.resize_uninitialized(return2.size());
             std::memcpy(buffer.data(), return2.data(), return2.size());
             handler.set_return_buffer(std::move(buffer), true);
             return {};
           }},
          // Level 3 - deepest level, just reverts
          {3,
           [data3, return3](SystemCallHandler &handler,
                            NativeRuntime &) -> InvokeStatus<void> {
             auto key = make_storage_key("level3");
             auto res =
                 handler.set_bytestring(key, std::as_bytes(std::span(data3)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Revert with data
             ReturnBuffer buffer;
             buffer.resize_uninitialized(return3.size());
             std::memcpy(buffer.data(), return3.data(), return3.size());
             handler.set_return_buffer(std::move(buffer), true);
             return {};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contract
  auto contract = make_native_contract(0);
  in_memory_db_.write_contract(sha256(*contract), contract);
  in_memory_db_.write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Invoke level 1
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(1),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);

  advance_block();

  // All levels should be reverted
  auto key1 = make_contract_data_addrkey(contract_addr, "level1");
  auto key2 = make_contract_data_addrkey(contract_addr, "level2");
  auto key3 = make_contract_data_addrkey(contract_addr, "level3");

  ASSERT_FALSE(lookup_storage_item_from_db(key1).has_value());
  ASSERT_FALSE(lookup_storage_item_from_db(key2).has_value());
  ASSERT_FALSE(lookup_storage_item_from_db(key3).has_value());
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
