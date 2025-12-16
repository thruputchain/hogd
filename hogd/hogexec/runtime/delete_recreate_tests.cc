#include <gtest/gtest.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/format.h"

namespace hogd {

class DeleteRecreateTest : public ::testing::TestWithParam<TestDB>,
                           public NativeContractTestFramework {
public:
  DeleteRecreateTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  void SetUp() override {}
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_P(DeleteRecreateTest, DeleteRecreateBytestring)
{
  // A native contract with the following methods:
  // 0: Reads a storage key, expecting it to be empty. Then writes a bytestring
  // to that same hardcoded storage key. 1: Deletes the storage key. 2: Calls 0,
  // then calls 1, then calls 0 again.
  //    Expected behavior: all three of those calls should succeed.
  auto make_context = [&]() -> std::unique_ptr<RuntimeContext> {
    auto native_contract_0 = std::make_unique<NativeContract>(
        std::unordered_map<MethodID, NativeContract::function_t>{
            {0,
             [](SystemCallHandler &handler,
                NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               auto key = make_storage_key("test");
               // Read the storage key, expecting it to be empty
               std::vector<std::byte> buf(1024);
               RuntimeMemoryLength len = 0;
               auto [status, _g1] = syscall_BytestringGet_inner(
                   handler, key, std::as_writable_bytes(std::span(buf)), len);
               if (!status.has_value()) {
                 ADD_FAILURE()
                     << "Failed to get bytestring: " << status.error();
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }
               if (status.value() !=
                   SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist) {
                 ADD_FAILURE() << "Expected storage key to be empty, but got: "
                               << x23::show(status.value());
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               // Write a bytestring to the storage key
               Bytestring bytestring = make_bytestring("hello");
               auto [write_status, _g2] = syscall_BytestringSet_inner(
                   handler, key, std::as_bytes(std::span(bytestring)));
               if (!write_status.has_value()) {
                 ADD_FAILURE()
                     << "Failed to write bytestring: " << write_status.error();
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }
               if (write_status.value() !=
                   SyscallErrorCode::kSyscallErrorCodeNone) {
                 ADD_FAILURE() << "Expected write to succeed, but got: "
                               << x23::show(write_status.value());
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               return {};
             }},
            {1,
             [](SystemCallHandler &handler,
                NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               auto key = make_storage_key("test");
               // Delete the storage key
               auto [delete_status, _g3] =
                   syscall_DeleteObject_inner(handler, key, 0);
               if (!delete_status.has_value()) {
                 ADD_FAILURE()
                     << "Failed to delete object: " << delete_status.error();
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }
               if (delete_status.value() !=
                   SyscallErrorCode::kSyscallErrorCodeNone) {
                 ADD_FAILURE() << "Expected delete to succeed, but got: "
                               << x23::show(delete_status.value());
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               return {};
             }},
            {2,
             [](SystemCallHandler &handler,
                NativeRuntime &runtime) -> InvokeStatus<void> {
               // Call method 0 (read empty, then write)
               auto [r0, _g0] = handler.invoke(
                   handler.get_current_address(), 0,
                   runtime.get_available_gas(), handler.get_calldata(),
                   std::to_underlying(InvokeFlag::kInvokeFlagNone));

               if (!r0.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }

               // Call method 1 (delete)
               auto [r1, _g1] =
                   handler.invoke(handler.get_current_address(), 1,
                                  runtime.get_available_gas(), Calldata{},
                                  std::to_underlying(
                                      InvokeFlag::kInvokeFlagNone));

               if (!r1.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }

               // Call method 0 again (read empty, then write)
               auto [r2, _g2] =
                   handler.invoke(handler.get_current_address(), 0,
                                  runtime.get_available_gas(), Calldata{},
                                  std::to_underlying(
                                      InvokeFlag::kInvokeFlagNone));

               if (!r2.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }

               return {};
             }}});
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_0));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  // mark the contract as existing and deployed within the database
  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);
  auto contract_0_addr = make_contract_address(0);
  write_deployment(contract_0_addr, contract_0);
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(contract_0_addr, 1'000'000'000);

  // now invoke the contract

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_0_addr,
                                 .entrypoint = entrypoint_from_method_id(2),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      make_context());

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events()());

  advance_block();

  // Verify that the bytestring was written (from the second call to method 0)
  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_0_addr, "test"));
  ASSERT_TRUE(read_res.has_value());
  auto expect = make_bytestring("hello");
  ASSERT_EQ(read_res.value().bytestring(), expect);
}

TEST_P(DeleteRecreateTest, DeleteRecreateNNInt)
{
  // A native contract with the following methods:
  // 0: Reads a storage key, expecting it to be empty. Then writes a nonnegative
  // integer to that same hardcoded storage key. 1: Deletes the storage key. 2:
  // Calls 0, then calls 1, then calls 0 again.
  //    Expected behavior: all three of those calls should succeed.
  auto make_context = [&]() -> std::unique_ptr<RuntimeContext> {
    auto native_contract_0 = std::make_unique<NativeContract>(
        std::unordered_map<MethodID, NativeContract::function_t>{
            {0,
             [](SystemCallHandler &handler,
                NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               auto key = make_storage_key("test");
               // Read the storage key, expecting it to be empty
               int128_t current_set = 0;
               bool current_set_opt = false;
               int128_t current_add = 0;
               auto [status, _g1] =
                   syscall_NonnegativeIntGet_inner(handler, key, true,
                                                   current_set, current_set_opt,
                                                   current_add);
               if (!status.has_value()) {
                 ADD_FAILURE() << "Failed to get nnint: " << status.error();
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }
               if (status.value() !=
                   SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist) {
                 ADD_FAILURE() << "Expected storage key to be empty, but got: "
                               << x23::show(status.value());
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               // Write a nonnegative integer to the storage key
               int128_t set_value = 42;
               auto [write_status, _g2] =
                   syscall_NonnegativeIntSetAdd_inner(handler, key, &set_value,
                                                      0);
               if (!write_status.has_value()) {
                 ADD_FAILURE()
                     << "Failed to write nnint: " << write_status.error();
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }
               if (write_status.value() !=
                   SyscallErrorCode::kSyscallErrorCodeNone) {
                 ADD_FAILURE() << "Expected write to succeed, but got: "
                               << x23::show(write_status.value());
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               return {};
             }},
            {1,
             [](SystemCallHandler &handler,
                NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               auto key = make_storage_key("test");
               // Delete the storage key
               auto [delete_status, _g3] =
                   syscall_DeleteObject_inner(handler, key, 0);
               if (!delete_status.has_value()) {
                 ADD_FAILURE()
                     << "Failed to delete object: " << delete_status.error();
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }
               if (delete_status.value() !=
                   SyscallErrorCode::kSyscallErrorCodeNone) {
                 ADD_FAILURE() << "Expected delete to succeed, but got: "
                               << x23::show(delete_status.value());
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               return {};
             }},
            {2,
             [](SystemCallHandler &handler,
                NativeRuntime &runtime) -> InvokeStatus<void> {
               // Call method 0 (read empty, then write)
               auto [r0, _g0] = handler.invoke(
                   handler.get_current_address(), 0,
                   runtime.get_available_gas(), handler.get_calldata(),
                   std::to_underlying(InvokeFlag::kInvokeFlagNone));

               if (!r0.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }

               // Call method 1 (delete)
               auto [r1, _g1] =
                   handler.invoke(handler.get_current_address(), 1,
                                  runtime.get_available_gas(), Calldata{},
                                  std::to_underlying(
                                      InvokeFlag::kInvokeFlagNone));

               if (!r1.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }

               // Call method 0 again (read empty, then write)
               auto [r2, _g2] =
                   handler.invoke(handler.get_current_address(), 0,
                                  runtime.get_available_gas(), Calldata{},
                                  std::to_underlying(
                                      InvokeFlag::kInvokeFlagNone));

               if (!r2.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }

               return {};
             }}});
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_0));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  // mark the contract as existing and deployed within the database
  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);
  auto contract_0_addr = make_contract_address(0);
  write_deployment(contract_0_addr, contract_0);
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(contract_0_addr, 1'000'000'000);

  // now invoke the contract

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_0_addr,
                                 .entrypoint = entrypoint_from_method_id(2),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      make_context());

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events()());

  advance_block();

  // Verify that the nnint was written (from the second call to method 0)
  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_0_addr, "test"));
  ASSERT_TRUE(read_res.has_value());
  ASSERT_EQ(read_res.value().type(), StorageItemType::kItemTypeNonnegativeInt);
  auto nnint_item = read_res.value().nnint();
  ASSERT_EQ(from_xdr(nnint_item), 42);
}

TEST_P(DeleteRecreateTest, DeleteRecreateRetrocausalQueue)
{
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP()
        << "Skipping test for HogDB due to retrocausal queue not supported";
    return;
  }
  // A native contract with the following methods:
  // 0: Consumes from a storage key, expecting it to be empty. Then inserts an
  // item into that same hardcoded storage key. 1: Deletes the storage key. 2:
  // Calls 0, then calls 1, then calls 0 again.
  //    Expected behavior: all three of those calls should succeed.
  auto make_context = [&]() -> std::unique_ptr<RuntimeContext> {
    auto native_contract_0 = std::make_unique<NativeContract>(
        std::unordered_map<MethodID, NativeContract::function_t>{
            {0,
             [](SystemCallHandler &handler,
                NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               auto key = make_storage_key("test");
               // Consume from the storage key, expecting it to be empty
               std::vector<std::byte> buf(1024);
               RuntimeMemoryLength written_len = 0;
               auto [status, _g1] = syscall_RetrocausalQueueConsume_inner(
                   handler, key, false, std::as_writable_bytes(std::span(buf)),
                   written_len);
               if (!status.has_value()) {
                 ADD_FAILURE()
                     << "Failed to consume from queue: " << status.error();
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }
               if (status.value() !=
                   SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry) {
                 ADD_FAILURE() << "Expected queue to be empty, but got: "
                               << x23::show(status.value());
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               // Insert an item into the queue
               QueueEntry entry = make_bytestring("item");
               auto [insert_status, _g2] = syscall_RetrocausalQueueInsert_inner(
                   handler, key, std::as_bytes(std::span(entry)));
               if (!insert_status.has_value()) {
                 ADD_FAILURE() << "Failed to insert into queue: "
                               << insert_status.error();
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }
               if (insert_status.value() !=
                   SyscallErrorCode::kSyscallErrorCodeNone) {
                 ADD_FAILURE() << "Expected insert to succeed, but got: "
                               << x23::show(insert_status.value());
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               return {};
             }},
            {1,
             [](SystemCallHandler &handler,
                NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               auto key = make_storage_key("test");
               // Delete the storage key
               auto [delete_status, _g3] =
                   syscall_DeleteObject_inner(handler, key, 0);
               if (!delete_status.has_value()) {
                 ADD_FAILURE()
                     << "Failed to delete object: " << delete_status.error();
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }
               if (delete_status.value() !=
                   SyscallErrorCode::kSyscallErrorCodeNone) {
                 ADD_FAILURE() << "Expected delete to succeed, but got: "
                               << x23::show(delete_status.value());
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               return {};
             }},
            {2,
             [](SystemCallHandler &handler,
                NativeRuntime &runtime) -> InvokeStatus<void> {
               // Call method 0 (consume empty, then insert)
               auto [r0, _g0] = handler.invoke(
                   handler.get_current_address(), 0,
                   runtime.get_available_gas(), handler.get_calldata(),
                   std::to_underlying(InvokeFlag::kInvokeFlagNone));

               if (!r0.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }

               // Call method 1 (delete)
               auto [r1, _g1] =
                   handler.invoke(handler.get_current_address(), 1,
                                  runtime.get_available_gas(), Calldata{},
                                  std::to_underlying(
                                      InvokeFlag::kInvokeFlagNone));

               if (!r1.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }

               // Call method 0 again (consume empty, then insert)
               auto [r2, _g2] =
                   handler.invoke(handler.get_current_address(), 0,
                                  runtime.get_available_gas(), Calldata{},
                                  std::to_underlying(
                                      InvokeFlag::kInvokeFlagNone));

               if (!r2.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }

               return {};
             }}});
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_0));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  // mark the contract as existing and deployed within the database
  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);
  auto contract_0_addr = make_contract_address(0);
  write_deployment(contract_0_addr, contract_0);
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(contract_0_addr, 1'000'000'000);

  // now invoke the contract

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_0_addr,
                                 .entrypoint = entrypoint_from_method_id(2),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      make_context());

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events()());

  advance_block();

  // Verify that the queue exists and has exactly one item (from the second call
  // to method 0)
  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_0_addr, "test"));
  ASSERT_TRUE(read_res.has_value());
  ASSERT_EQ(read_res.value().type(),
            StorageItemType::kItemTypeRetrocausalQueue);

  // Verify there's exactly one item by consuming: first consume should succeed,
  // second should fail
  auto verify_context = [&]() -> std::unique_ptr<RuntimeContext> {
    auto native_contract_0 = std::make_unique<NativeContract>(
        std::unordered_map<MethodID, NativeContract::function_t>{
            {0,
             [](SystemCallHandler &handler,
                NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               auto key = make_storage_key("test");
               std::vector<std::byte> buf(1024);
               RuntimeMemoryLength written_len = 0;
               // First consume should succeed
               auto [status1, _g1] = syscall_RetrocausalQueueConsume_inner(
                   handler, key, false, std::as_writable_bytes(std::span(buf)),
                   written_len);
               if (!status1.has_value() ||
                   status1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
               // Second consume should fail (queue empty)
               auto [status2, _g2] = syscall_RetrocausalQueueConsume_inner(
                   handler, key, false, std::as_writable_bytes(std::span(buf)),
                   written_len);
               if (!status2.has_value() ||
                   status2.value() !=
                       SyscallErrorCode::
                           kSyscallErrorCodeNoRetrocausalQueueEntry) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
               return {};
             }}});
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_0));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto verify_handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_0_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      verify_context());

  auto verify_res = verify_handler.root_invoke();
  ASSERT_TRUE(verify_res.is_root_success());
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, DeleteRecreateTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
