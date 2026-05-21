#include <gtest/gtest.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// NOLINTNEXTLINE(misc-use-internal-linkage)
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
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [str_hello, key](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             // Write the bytestring to storage
             auto [res, _gas] = syscall_BytestringSet_inner(
                 ctx.handler(), key, std::as_bytes(std::span(str_hello)),
                 std::to_underlying(StorageDeltaFlags::None));
             if (!res.has_value() ||
                 res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               ADD_FAILURE()
                   << "RootFrameReverts: Failed to set bytestring in method 0";
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             // Now revert the transaction
             ctx.handler().set_return_buffer(ReturnBuffer{}, true);
             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Deploy contract and grant storage tokens
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);
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
      *runtime_context);

  // Execute the transaction - it should succeed
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  // Commit the transaction - it should succeed
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultTxRootFrameReverted);

  advance_block();

  // Check that the bytestring was NOT written (due to the revert)
  auto data_addrkey = make_contract_data_addrkey(contract_addr, "test");
  auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
  ASSERT_FALSE(data_item_opt.has_value());
}

// Test cross-contract nested frame revert with try_invoke.
//
// Four contracts in a call chain (no reentrance):
//
//   addr_0:0 --try_invoke--> addr_1:0 --try_invoke--> addr_2:0 --try_invoke-->
//   addr_3:0
//                             (reverts)                (writes "test1") (writes
//                             "test2")
//
// addr_1 manually reverts after addr_2's try_invoke succeeds. This should
// undo all state changes from addr_2 and addr_3.
//
// After the revert, addr_0 invokes check methods on addr_2 and addr_3 that
// verify the bytestrings are absent from their own storage.
//
// Post-commit: neither bytestring is written.
TEST_P(RevertTest, NestedFrameWithTryInvokeReverts)
{
  ContractAddress addr_0 = make_contract_address(0);
  ContractAddress addr_1 = make_contract_address(1);
  ContractAddress addr_2 = make_contract_address(2);
  ContractAddress addr_3 = make_contract_address(3);

  auto str_hello = make_bytestring("hello");
  auto str_world = make_bytestring("world");
  auto key1 = make_storage_key("test1");
  auto key2 = make_storage_key("test2");

  // Contract 0 (addr_0): try_invoke addr_1:0, assert FrameReverted,
  // then invoke check methods on addr_2 and addr_3 to verify writes are gone.
  auto contract_0 = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [addr_1, addr_2,
            addr_3](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             // try_invoke addr_1:0
             auto r1 = [&]() {
               RuntimeMemoryLength output_return_buffer_len = 0,
                                   output_returned_auth_token_len = 0;
               return ctx.syscall<SyscallNo::kInvoke>(
                   addr_1, 0, UINT64_MAX, Calldata{},
                   std::to_underlying(InvokeFlag::TryInvoke),
                   std::span<uint32_t>{}, output_return_buffer_len,
                   output_returned_auth_token_len);
             }();
             if (!r1.has_value()) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             if (r1.value() !=
                 SyscallErrorCode::kSyscallErrorCodeFrameReverted) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             // Invoke addr_2:1 (check method) to verify "test1" is absent
             auto r2_check = [&]() {
               RuntimeMemoryLength output_return_buffer_len = 0,
                                   output_returned_auth_token_len = 0;
               return ctx.syscall<SyscallNo::kInvoke>(
                   addr_2, 1, UINT64_MAX, Calldata{},
                   std::to_underlying(InvokeFlag::None), std::span<uint32_t>{},
                   output_return_buffer_len, output_returned_auth_token_len);
             }();
             if (!r2_check.has_value() ||
                 r2_check.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               ADD_FAILURE()
                   << "NestedFrameWithTryInvokeReverts: addr_2 check failed";
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             // Invoke addr_3:1 (check method) to verify "test2" is absent
             auto r3_check = [&]() {
               RuntimeMemoryLength output_return_buffer_len = 0,
                                   output_returned_auth_token_len = 0;
               return ctx.syscall<SyscallNo::kInvoke>(
                   addr_3, 1, UINT64_MAX, Calldata{},
                   std::to_underlying(InvokeFlag::None), std::span<uint32_t>{},
                   output_return_buffer_len, output_returned_auth_token_len);
             }();
             if (!r3_check.has_value() ||
                 r3_check.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               ADD_FAILURE()
                   << "NestedFrameWithTryInvokeReverts: addr_3 check failed";
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             return {};
           }}});

  // Contract 1 (addr_1): try_invoke addr_2:0, assert success, then manually
  // revert. This undoes all state changes from addr_2 and addr_3.
  auto contract_1 = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [addr_2](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto r2 = [&]() {
               RuntimeMemoryLength output_return_buffer_len = 0,
                                   output_returned_auth_token_len = 0;
               return ctx.syscall<SyscallNo::kInvoke>(
                   addr_2, 0, UINT64_MAX, Calldata{},
                   std::to_underlying(InvokeFlag::TryInvoke),
                   std::span<uint32_t>{}, output_return_buffer_len,
                   output_returned_auth_token_len);
             }();
             if (!r2.has_value()) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             if (r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             // Manually revert -- undoes locks acquired by addr_2 and addr_3
             ctx.handler().set_return_buffer(ReturnBuffer{}, true);
             return {};
           }}});

  // Contract 2 (addr_2):
  //   method 0: write "hello" to "test1", try_invoke addr_3:0
  //   method 1: check that "test1" is absent (verification after revert)
  auto contract_2 = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [str_hello, key1,
            addr_3](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto [res, _gas] = syscall_BytestringSet_inner(
                 ctx.handler(), key1, std::as_bytes(std::span(str_hello)),
                 std::to_underlying(StorageDeltaFlags::None));
             if (!res.has_value() ||
                 res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               ADD_FAILURE() << "NestedFrameWithTryInvokeReverts: Failed to "
                                "set bytestring in contract 2";
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             auto r3 = [&]() {
               RuntimeMemoryLength output_return_buffer_len = 0,
                                   output_returned_auth_token_len = 0;
               return ctx.syscall<SyscallNo::kInvoke>(
                   addr_3, 0, UINT64_MAX, Calldata{},
                   std::to_underlying(InvokeFlag::TryInvoke),
                   std::span<uint32_t>{}, output_return_buffer_len,
                   output_returned_auth_token_len);
             }();
             if (!r3.has_value()) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             if (r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             return {};
           }},
          {1, [key1](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto res = ctx.handler().get_bytestring(key1);
             if (!res.has_value()) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             if (res.value().has_value()) {
               ADD_FAILURE()
                   << "NestedFrameWithTryInvokeReverts: test1 should be "
                      "absent after revert";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }}});

  // Contract 3 (addr_3):
  //   method 0: write "world" to "test2"
  //   method 1: check that "test2" is absent (verification after revert)
  auto contract_3 = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [str_world, key2](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto [res, _gas] = syscall_BytestringSet_inner(
                 ctx.handler(), key2, std::as_bytes(std::span(str_world)),
                 std::to_underlying(StorageDeltaFlags::None));
             if (!res.has_value() ||
                 res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               ADD_FAILURE() << "NestedFrameWithTryInvokeReverts: Failed to "
                                "set bytestring in contract 3";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }},
          {1, [key2](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto res = ctx.handler().get_bytestring(key2);
             if (!res.has_value()) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             if (res.value().has_value()) {
               ADD_FAILURE()
                   << "NestedFrameWithTryInvokeReverts: test2 should be "
                      "absent after revert";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract_0));
  contract_map.emplace(1, std::move(contract_1));
  contract_map.emplace(2, std::move(contract_2));
  contract_map.emplace(3, std::move(contract_3));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Deploy all four contracts and grant storage tokens
  ContractAddress addrs[] = {addr_0, addr_1, addr_2, addr_3};
  for (int i = 0; i < 4; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  // Invoke contract 0 method 0
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = addr_0,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  advance_block();

  // Post-commit: neither bytestring should have been written
  auto data_addrkey1 = make_contract_data_addrkey(addr_2, "test1");
  auto data_item1_opt = lookup_storage_item_from_db(data_addrkey1);
  ASSERT_FALSE(data_item1_opt.has_value());

  auto data_addrkey2 = make_contract_data_addrkey(addr_3, "test2");
  auto data_item2_opt = lookup_storage_item_from_db(data_addrkey2);
  ASSERT_FALSE(data_item2_opt.has_value());
}

TEST_P(RevertTest, GetReturnBufferOnFault)
{
  // Two native contracts (no self-reentrance):
  //
  //   addr_fault:0    immediately returns a deterministic fault
  //   addr_caller:0   invokes addr_fault:0, then
  //   syscall_GetReturnedBuffer_inner
  //
  // The caller should observe kSyscallErrorCodeNoReturnBufferAvailable after
  // the nested invoke faults.

  ContractAddress addr_fault = make_contract_address(2);
  ContractAddress addr_caller = make_contract_address(3);

  auto contract_fault = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext & /*unused*/) -> InvokeStatus<void> {
             return InvokeError{InvokeErrorCode::Deterministic};
           }}});

  auto contract_caller = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [addr_fault](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto r0 = [&]() {
               RuntimeMemoryLength output_return_buffer_len = 0,
                                   output_returned_auth_token_len = 0;
               return ctx.syscall<SyscallNo::kInvoke>(
                   addr_fault, 0, UINT64_MAX, Calldata{},
                   std::to_underlying(InvokeFlag::None), std::span<uint32_t>{},
                   output_return_buffer_len, output_returned_auth_token_len);
             }();
             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             if (r0.value() !=
                 SyscallErrorCode::kSyscallErrorCodeDeterministicWasm) {
               ADD_FAILURE()
                   << "GetReturnBufferOnFault: Expected nested invoke "
                      "to return kSyscallErrorCodeDeterministicWasm";
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             ReturnBuffer buffer;
             RuntimeMemoryLength written_len = 0;
             std::array<std::byte, 128> restored_auth_token_ids{};
             RuntimeMemoryLength restored_auth_token_ids_written_len = 0;

             auto [res, _gas] = syscall_GetReturnedBuffer_inner(
                 ctx.handler(), std::as_writable_bytes(std::span(buffer)),
                 written_len, std::span(restored_auth_token_ids),
                 restored_auth_token_ids_written_len);
             if (!res.has_value()) {
               ADD_FAILURE()
                   << "GetReturnBufferOnFault: Failed to get return buffer";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             if (res.value() !=
                 SyscallErrorCode::kSyscallErrorCodeNoReturnBufferAvailable) {
               ADD_FAILURE()
                   << "GetReturnBufferOnFault: Expected "
                      "kSyscallErrorCodeNoReturnBufferAvailable, got: "
                   << static_cast<int32_t>(res.value());
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract_fault));
  contract_map.emplace(1, std::move(contract_caller));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Deploy both contracts and grant storage tokens
  ContractAddress addrs[] = {addr_fault, addr_caller};
  for (int i = 0; i < 2; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  // Invoke caller method 0
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = addr_caller,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *runtime_context);

  // Execute the transaction - it should succeed
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  // Commit the transaction - it should succeed
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  advance_block();
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, RevertTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
