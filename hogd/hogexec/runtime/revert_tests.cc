#include <gtest/gtest.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "rpc_types.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
class RevertTest : public ::testing::TestWithParam<TestDB>,
                   public NativeContractTestFramework {
public:
  RevertTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// Check on how a root frame reverts,
// when the reverting frame is the root frame.
TEST_P(RevertTest, RootFrameReverts)
{
  // Native contract with one method.  The method performs a bytestring write,
  // and then the contract reverts.
  // Test should check that the contract executes successfully, and the
  // transaction commits successfully, but the bytestring is not written.

  // Contract address
  ContractAddress contract_addr = make_contract_address(1);

  // Bytestring to write
  auto str_hello = make_bytestring("hello");
  auto key = make_storage_key("test");

  // Deploy contract with one method that writes a bytestring and then reverts
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [str_hello, key](SystemCallHandler &handler,
                            NativeRuntime &) -> InvokeStatus<void> {
             // Write the bytestring to storage
             auto res =
                 handler.set_bytestring(key,
                                        std::as_bytes(std::span(str_hello)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Now revert the transaction
             handler.set_return_buffer(ReturnBuffer{}, true);
             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contract and grant storage tokens
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Invoke the contract method
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);

  // Execute the transaction - it should succeed
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  // Commit the transaction - it should succeed
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);

  advance_block();

  // Check that the bytestring was NOT written (due to the revert)
  auto data_addrkey = make_contract_data_addrkey(contract_addr, "test");
  auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
  ASSERT_FALSE(data_item_opt.has_value());
}

// Check on how nested frames revert,
// when the reverting frame is not the root frame.
TEST_P(RevertTest, NestedFrameWithTryInvokeReverts)
{
  // Native contract with four methods.  The first method performs a try_invoke
  // on the second method.  The second method performs a try_invoke on the
  // third, and then reverts.  The third method performs a bytestring write,
  // then performs a try_invoke on the fourth method, and then returns success.
  // The fourth method performs a bytestring write, and then returns success.
  // returns success.
  //
  // Test should check that the contract executes successfully, and the
  // transaction commits successfully, but neither of the bytestrings are
  // written.

  // Contract address
  ContractAddress contract_addr = make_contract_address(2);

  // Bytestrings to write
  auto str_hello = make_bytestring("hello");
  auto str_world = make_bytestring("world");
  auto key1 = make_storage_key("test1");
  auto key2 = make_storage_key("test2");

  // Deploy contract with four methods:
  // 0: try_invoke method 1
  // 1: try_invoke method 2, then revert
  // 2: Write "hello" to storage, try_invoke method 3, then return success
  // 3: Write "world" to storage, then return success
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Try invoke method 1
             auto [r1, _g1] = handler.invoke(
                 handler.get_current_address(), 1, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r1.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Method 1 should succeed, but state changes are gone because of
             // the manual revert
             if (r1.value() != SyscallErrorCode::kFrameReverted) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Check that neither bytestring was written
             auto key1 = make_storage_key("test1");
             auto key2 = make_storage_key("test2");

             auto res1 = handler.get_bytestring(key1);
             if (!res1.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (res1.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             auto res2 = handler.get_bytestring(key2);
             if (!res2.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (res2.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }},
          {1,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Try invoke method 2
             auto [r2, _g2] = handler.invoke(
                 handler.get_current_address(), 2, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r2.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Method 2 should succeed in the try_invoke frame
             if (r2.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Now manually revert this frame, which should undo the locks
             // acquired by method 2 and method 3
             handler.set_return_buffer(ReturnBuffer{}, true);
             return {};
           }},
          {2,
           [str_hello, key1](SystemCallHandler &handler,
                             NativeRuntime &runtime) -> InvokeStatus<void> {
             // Write "hello" to storage
             auto res =
                 handler.set_bytestring(key1,
                                        std::as_bytes(std::span(str_hello)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Try invoke method 3
             auto [r3, _g3] = handler.invoke(
                 handler.get_current_address(), 3, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r3.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Method 3 should succeed
             if (r3.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }},
          {3,
           [str_world, key2](SystemCallHandler &handler,
                             NativeRuntime &) -> InvokeStatus<void> {
             // Write "world" to storage
             auto res =
                 handler.set_bytestring(key2,
                                        std::as_bytes(std::span(str_world)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contract and grant storage tokens
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Invoke method 0
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);

  // Execute the transaction - it should succeed
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  // Commit the transaction - it should succeed
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);

  advance_block();

  // Check that neither bytestring was written (due to the revert)
  auto data_addrkey1 = make_contract_data_addrkey(contract_addr, "test1");
  auto data_item1_opt = lookup_storage_item_from_db(data_addrkey1);
  ASSERT_FALSE(data_item1_opt.has_value());

  auto data_addrkey2 = make_contract_data_addrkey(contract_addr, "test2");
  auto data_item2_opt = lookup_storage_item_from_db(data_addrkey2);
  ASSERT_FALSE(data_item2_opt.has_value());
}
// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, RevertTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
