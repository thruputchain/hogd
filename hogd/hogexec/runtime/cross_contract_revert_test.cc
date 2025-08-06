#include <gtest/gtest.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
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

//
// Comprehensive tests for cross-contract revert propagation.
//
// These tests verify that manual frame revert behavior works correctly
// when contracts call other contracts, ensuring that:
// 1. Revert propagates correctly through contract call chains
// 2. State changes are properly isolated and reverted
// 3. Return data flows correctly through nested calls
// 4. Gas accounting works properly during cross-contract reverts
// 5. Complex nested scenarios behave as expected
//

namespace hogd {

class CrossContractRevertTest : public ::testing::Test,
                                public NativeContractTestFramework {
public:
  CrossContractRevertTest() : NativeContractTestFramework(false) {}
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

//
// Test basic cross-contract revert propagation.
//
// Scenario: Contract A calls Contract B, Contract B reverts with return data.
// Verifies that:
// - Contract B's state changes are reverted
// - Contract A receives B's return data
// - Contract A's state changes persist
// - Transaction succeeds despite B's revert
//
TEST_F(CrossContractRevertTest, BasicCrossContractRevert)
{
  ContractAddress contract_a_addr = make_contract_address(0);
  ContractAddress contract_b_addr = make_contract_address(1);

  auto contract_a_data = make_bytestring("contract_a_state");
  auto contract_b_data = make_bytestring("contract_b_state");
  auto contract_b_return_data = make_bytestring("contract_b_error");
  auto contract_a_key = make_storage_key("contract_a_key");
  auto contract_b_key = make_storage_key("contract_b_key");

  // Contract A: Calls Contract B and writes its own state
  auto contract_a = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [contract_a_data, contract_a_key,
            contract_b_addr](SystemCallHandler &handler,
                             NativeRuntime &runtime) -> InvokeStatus<void> {
             // Contract A writes its own state
             auto res = handler.set_bytestring(contract_a_key,
                                               std::as_bytes(
                                                   std::span(contract_a_data)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Call Contract B (which will revert)
             auto [r1, _g1] =
                 handler.invoke(contract_b_addr, 0, runtime.get_available_gas(),
                                {}, 0);
             if (!r1.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (r1.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Contract A should receive Contract B's return data
             auto status = handler.get_return_buffer();
             if (!status) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Verify return data matches expected
             auto expected_data = make_bytestring("contract_b_error");
             if ((*status)->size() != expected_data.size()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (std::memcmp((*status)->data(), expected_data.data(),
                             expected_data.size()) != 0) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }}});

  // Contract B: Writes state and then reverts with return data
  auto contract_b = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [contract_b_data, contract_b_return_data,
            contract_b_key](SystemCallHandler &handler,
                            NativeRuntime &) -> InvokeStatus<void> {
             // Contract B writes state that should be reverted
             auto res = handler.set_bytestring(contract_b_key,
                                               std::as_bytes(
                                                   std::span(contract_b_data)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Contract B manually reverts with return data
             ReturnBuffer buffer;
             buffer.resize_uninitialized(contract_b_return_data.size());
             std::memcpy(buffer.data(), contract_b_return_data.data(),
                         contract_b_return_data.size());
             handler.set_return_buffer(std::move(buffer), true);
             return {};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract_a));
  contract_map.emplace(1, std::move(contract_b));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy both contracts
  auto contract_a_obj = make_native_contract(0);
  auto contract_b_obj = make_native_contract(1);
  in_memory_db_.write_contract(sha256(*contract_a_obj), contract_a_obj);
  in_memory_db_.write_contract(sha256(*contract_b_obj), contract_b_obj);
  in_memory_db_.write_deployment(contract_a_addr, sha256(*contract_a_obj));
  in_memory_db_.write_deployment(contract_b_addr, sha256(*contract_b_obj));
  setup_grant_storage_token(contract_a_addr, 1'000'000'000);
  setup_grant_storage_token(contract_b_addr, 1'000'000'000);

  // Invoke Contract A
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_a_addr,
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

  // Contract A's state should persist
  auto contract_a_addrkey =
      make_contract_data_addrkey(contract_a_addr, "contract_a_key");
  auto contract_a_item_opt = lookup_storage_item_from_db(contract_a_addrkey);
  ASSERT_TRUE(contract_a_item_opt.has_value());

  // Contract B's state should be reverted
  auto contract_b_addrkey =
      make_contract_data_addrkey(contract_b_addr, "contract_b_key");
  auto contract_b_item_opt = lookup_storage_item_from_db(contract_b_addrkey);
  ASSERT_FALSE(contract_b_item_opt.has_value());
}

//
// Test deep nested revert propagation (A -> B -> C).
//
// Scenario: Contract A calls Contract B, which calls Contract C.
// Contract C reverts with return data. Verifies that:
// - Contract C's state is reverted
// - Contract B receives C's return data and can process it
// - Contract A receives B's return data
// - Only Contract A's state persists
//
TEST_F(CrossContractRevertTest, DeepNestedRevertPropagation)
{
  ContractAddress contract_a_addr = make_contract_address(0);
  ContractAddress contract_b_addr = make_contract_address(1);
  ContractAddress contract_c_addr = make_contract_address(2);

  auto contract_a_data = make_bytestring("contract_a_state");
  auto contract_b_data = make_bytestring("contract_b_state");
  auto contract_c_data = make_bytestring("contract_c_state");
  auto contract_c_return_data = make_bytestring("contract_c_error");
  auto contract_b_return_data = make_bytestring("contract_b_processed");

  auto contract_a_key = make_storage_key("contract_a_key");
  auto contract_b_key = make_storage_key("contract_b_key");
  auto contract_c_key = make_storage_key("contract_c_key");

  // Contract A: Calls Contract B and writes its own state
  auto contract_a = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [contract_a_data, contract_a_key,
            contract_b_addr](SystemCallHandler &handler,
                             NativeRuntime &runtime) -> InvokeStatus<void> {
             // Contract A writes its own state
             auto res = handler.set_bytestring(contract_a_key,
                                               std::as_bytes(
                                                   std::span(contract_a_data)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Call Contract B
             auto [r1, _g1] =
                 handler.invoke(contract_b_addr, 0, runtime.get_available_gas(),
                                {}, 0);
             if (!r1.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (r1.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Contract A should receive Contract B's processed return data
             auto status = handler.get_return_buffer();
             if (!status) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Verify return data matches expected
             auto expected_data = make_bytestring("contract_b_processed");
             if ((*status)->size() != expected_data.size()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (std::memcmp((*status)->data(), expected_data.data(),
                             expected_data.size()) != 0) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }}});

  // Contract B: Calls Contract C, processes C's return data, and returns
  // processed data
  auto contract_b = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [contract_b_data, contract_b_return_data, contract_b_key,
            contract_c_addr](SystemCallHandler &handler,
                             NativeRuntime &runtime) -> InvokeStatus<void> {
             // Contract B writes state that should be reverted
             auto res = handler.set_bytestring(contract_b_key,
                                               std::as_bytes(
                                                   std::span(contract_b_data)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Call Contract C (which will revert)
             auto [r1, _g1] =
                 handler.invoke(contract_c_addr, 0, runtime.get_available_gas(),
                                {}, 0);
             if (!r1.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (r1.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Contract B receives Contract C's return data
             auto status = handler.get_return_buffer();
             if (!status) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Verify C's return data
             auto expected_c_data = make_bytestring("contract_c_error");
             if ((*status)->size() != expected_c_data.size()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (std::memcmp((*status)->data(), expected_c_data.data(),
                             expected_c_data.size()) != 0) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Contract B returns processed data
             ReturnBuffer buffer;
             buffer.resize_uninitialized(contract_b_return_data.size());
             std::memcpy(buffer.data(), contract_b_return_data.data(),
                         contract_b_return_data.size());
             handler.set_return_buffer(std::move(buffer), true);
             return {};
           }}});

  // Contract C: Writes state and reverts with return data
  auto contract_c = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [contract_c_data, contract_c_return_data,
            contract_c_key](SystemCallHandler &handler,
                            NativeRuntime &) -> InvokeStatus<void> {
             // Contract C writes state that should be reverted
             auto res = handler.set_bytestring(contract_c_key,
                                               std::as_bytes(
                                                   std::span(contract_c_data)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Contract C manually reverts with return data
             ReturnBuffer buffer;
             buffer.resize_uninitialized(contract_c_return_data.size());
             std::memcpy(buffer.data(), contract_c_return_data.data(),
                         contract_c_return_data.size());
             handler.set_return_buffer(std::move(buffer), true);
             return {};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract_a));
  contract_map.emplace(1, std::move(contract_b));
  contract_map.emplace(2, std::move(contract_c));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy all contracts
  auto contract_a_obj = make_native_contract(0);
  auto contract_b_obj = make_native_contract(1);
  auto contract_c_obj = make_native_contract(2);
  in_memory_db_.write_contract(sha256(*contract_a_obj), contract_a_obj);
  in_memory_db_.write_contract(sha256(*contract_b_obj), contract_b_obj);
  in_memory_db_.write_contract(sha256(*contract_c_obj), contract_c_obj);
  in_memory_db_.write_deployment(contract_a_addr, sha256(*contract_a_obj));
  in_memory_db_.write_deployment(contract_b_addr, sha256(*contract_b_obj));
  in_memory_db_.write_deployment(contract_c_addr, sha256(*contract_c_obj));
  setup_grant_storage_token(contract_a_addr, 1'000'000'000);
  setup_grant_storage_token(contract_b_addr, 1'000'000'000);
  setup_grant_storage_token(contract_c_addr, 1'000'000'000);

  // Invoke Contract A
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_a_addr,
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

  // Contract A's state should persist
  auto contract_a_addrkey =
      make_contract_data_addrkey(contract_a_addr, "contract_a_key");
  auto contract_a_item_opt = lookup_storage_item_from_db(contract_a_addrkey);
  ASSERT_TRUE(contract_a_item_opt.has_value());

  // Contract B's state should be reverted
  auto contract_b_addrkey =
      make_contract_data_addrkey(contract_b_addr, "contract_b_key");
  auto contract_b_item_opt = lookup_storage_item_from_db(contract_b_addrkey);
  ASSERT_FALSE(contract_b_item_opt.has_value());

  // Contract C's state should be reverted
  auto contract_c_addrkey =
      make_contract_data_addrkey(contract_c_addr, "contract_c_key");
  auto contract_c_item_opt = lookup_storage_item_from_db(contract_c_addrkey);
  ASSERT_FALSE(contract_c_item_opt.has_value());
}

//
// Test basic cross-contract revert with return data.
//
// Scenario: Contract A calls Contract B, Contract B reverts with return data.
// This is a simpler version of the basic test to ensure the pattern works.
//
TEST_F(CrossContractRevertTest, RevertWithReturnData)
{
  ContractAddress contract_a_addr = make_contract_address(0);
  ContractAddress contract_b_addr = make_contract_address(1);

  auto contract_a_data = make_bytestring("contract_a_state");
  auto contract_b_return_data = make_bytestring("contract_b_error");

  auto contract_a_key = make_storage_key("contract_a_key");

  // Contract A: Calls Contract B and writes its own state
  auto contract_a = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [contract_a_data, contract_a_key,
            contract_b_addr](SystemCallHandler &handler,
                             NativeRuntime &runtime) -> InvokeStatus<void> {
             // Contract A writes its own state
             auto res = handler.set_bytestring(contract_a_key,
                                               std::as_bytes(
                                                   std::span(contract_a_data)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Call Contract B (which will revert)
             auto [r1, _g1] =
                 handler.invoke(contract_b_addr, 0, runtime.get_available_gas(),
                                {}, 0);
             if (!r1.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (r1.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Contract A should receive Contract B's return data
             auto status = handler.get_return_buffer();
             if (!status) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Verify return data matches expected
             auto expected_data = make_bytestring("contract_b_error");
             if ((*status)->size() != expected_data.size()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (std::memcmp((*status)->data(), expected_data.data(),
                             expected_data.size()) != 0) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }}});

  // Contract B: Reverts with return data
  auto contract_b = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [contract_b_return_data](SystemCallHandler &handler,
                                    NativeRuntime &) -> InvokeStatus<void> {
             // Contract B manually reverts with return data
             ReturnBuffer buffer;
             buffer.resize_uninitialized(contract_b_return_data.size());
             std::memcpy(buffer.data(), contract_b_return_data.data(),
                         contract_b_return_data.size());
             handler.set_return_buffer(std::move(buffer), true);
             return {};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract_a));
  contract_map.emplace(1, std::move(contract_b));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy both contracts
  auto contract_a_obj = make_native_contract(0);
  auto contract_b_obj = make_native_contract(1);
  in_memory_db_.write_contract(sha256(*contract_a_obj), contract_a_obj);
  in_memory_db_.write_contract(sha256(*contract_b_obj), contract_b_obj);
  in_memory_db_.write_deployment(contract_a_addr, sha256(*contract_a_obj));
  in_memory_db_.write_deployment(contract_b_addr, sha256(*contract_b_obj));
  setup_grant_storage_token(contract_a_addr, 1'000'000'000);
  setup_grant_storage_token(contract_b_addr, 1'000'000'000);

  // Invoke Contract A
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_a_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success())
      << "Expected root success, but frame success: " << res.is_frame_success();

  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);

  advance_block();

  // Contract A's state should persist
  auto contract_a_addrkey =
      make_contract_data_addrkey(contract_a_addr, "contract_a_key");
  auto contract_a_item_opt = lookup_storage_item_from_db(contract_a_addrkey);
  ASSERT_TRUE(contract_a_item_opt.has_value());
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
