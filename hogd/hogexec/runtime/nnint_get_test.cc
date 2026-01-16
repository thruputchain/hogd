#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "rpc_types.hh"
#include "storage_delta_flags.hh"
#include "storage_item.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// Tests for nnint get syscall with force_load_snapshot=false.
// This tests edge cases where we don't force loading from the snapshot,
// focusing on the behavior when keys don't exist, are deleted, have different
// types, or have been written to in the current transaction.

class NNIntGetTest : public ::testing::TestWithParam<TestDB>,
                     public NativeContractTestFramework {
public:
  NNIntGetTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  static InvokeStatus<void> check_nnint_get_result(
      SystemStatus<SyscallStatus<std::pair<std::optional<int128_t>, int128_t>>>
          res,
      std::optional<int128_t> expected_base, int128_t expected_add)
  {
    if (!res.has_value()) {
      return res.error().to_invoke_error();
    }
    if (!res.value().has_value()) {
      return InvokeError{InvokeErrorCode::kInvokeDeterministic};
    }
    if (res.value().value().first != expected_base) {
      return InvokeError{InvokeErrorCode::kInvokeDeterministic};
    }
    if (res.value().value().second != expected_add) {
      return InvokeError{InvokeErrorCode::kInvokeDeterministic};
    }
    return {};
  }
};

// Test: Get on a key that doesn't exist (UnmodifiedItem, not loaded).
// With force_load_snapshot=false, should return (nullopt, 0) without
// querying the database.
TEST_P(NNIntGetTest, GetOnNonExistentKey)
{
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("nonexistent");
             auto res = handler.get_nnint(key, false);
             return check_nnint_get_result(res, std::nullopt, 0);
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);

  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test: Get on a key that was deleted (DeletedItem).
// With force_load_snapshot=false, should return (nullopt, 0).
TEST_P(NNIntGetTest, GetOnDeletedKey)
{
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("deleted_key");
             // First, set a value
             int128_t set_value = 100;
             auto [r, _gas] = syscall_NonnegativeIntSetAdd_inner(
                 handler, key, &set_value, 50,
                 std::to_underlying(StorageItemRPCFlags::kStorageNone));
             if (!r.has_value() ||
                 r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Then delete it
             auto [r2, _gas2] = syscall_DeleteObject_inner(
                 handler, key,
                 std::to_underlying(StorageItemRPCFlags::kStorageNone));
             if (!r2.has_value() ||
                 r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Get with force_load_snapshot=false should return (nullopt, 0)
             auto res = handler.get_nnint(key, false);
             return check_nnint_get_result(res, std::nullopt, 0);
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);

  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test: Get on a key that has an NNInt write with base_value.
// Should return (base_value, add_value).
TEST_P(NNIntGetTest, GetOnNNIntWithBaseValue)
{
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("nnint_with_base");
             int128_t set_value = 200;
             auto [r, _gas] = syscall_NonnegativeIntSetAdd_inner(
                 handler, key, &set_value, 50,
                 std::to_underlying(StorageItemRPCFlags::kStorageNone));
             if (!r.has_value() ||
                 r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Get with force_load_snapshot=false should return (200, 50)
             auto res = handler.get_nnint(key, false);
             return check_nnint_get_result(res, 200, 50);
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);

  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test: Get on a key that has an NNInt write without base_value (just add).
// Should return (nullopt, add_value).
TEST_P(NNIntGetTest, GetOnNNIntWithoutBaseValue)
{
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("nnint_add_only");
             // Set with nullopt base, just add value
             auto [r, _gas] = syscall_NonnegativeIntSetAdd_inner(
                 handler, key, nullptr, 75,
                 std::to_underlying(StorageItemRPCFlags::kStorageNone));
             if (!r.has_value() ||
                 r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Get with force_load_snapshot=false should return (nullopt, 75)
             auto res = handler.get_nnint(key, false);
             return check_nnint_get_result(res, std::nullopt, 75);
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);

  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test: Get on a key that has a bytestring write (different type).
// With force_load_snapshot=false, should return (nullopt, 0).
TEST_P(NNIntGetTest, GetOnBytestringKey)
{
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("bytestring_key");
             // First, set a bytestring value
             Bytestring bytestring_data = make_bytestring("test_data");
             auto [r, _gas] = syscall_BytestringSet_inner(
                 handler, key, std::as_bytes(std::span(bytestring_data)),
                 std::to_underlying(StorageItemRPCFlags::kStorageNone));
             if (!r.has_value() ||
                 r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Get with force_load_snapshot=false should return (nullopt, 0)
             // because the type doesn't match
             auto res = handler.get_nnint(key, false);
             return check_nnint_get_result(res, std::nullopt, 0);
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);

  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test: Get on a key that exists in snapshot but hasn't been accessed yet.
// With force_load_snapshot=false, should return (nullopt, 0) without
// loading from snapshot.
TEST_P(NNIntGetTest, GetOnUnmodifiedKeyInSnapshot)
{
  auto contract_addr = make_contract_address(0);
  auto key = make_contract_data_addrkey(contract_addr, "snapshot_key");

  // First, write an NNInt to the database in a previous block
  write_item(key,
             std::make_shared<StorageItemWrapper>(300,
                                                  get_default_cost_model()));
  advance_block();

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("snapshot_key");
             // Get with force_load_snapshot=false should return (nullopt, 0)
             // without loading from snapshot
             auto res = handler.get_nnint(key, false);
             return check_nnint_get_result(res, std::nullopt, 0);
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test: Multiple operations - set with base, then add, then get.
// Verifies that the latest write is what's returned.
TEST_P(NNIntGetTest, MultipleOperationsSequence)
{
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("multi_op_key");
             // First set with base value
             int128_t set_value = 100;
             auto [r1, _gas1] = syscall_NonnegativeIntSetAdd_inner(
                 handler, key, &set_value, 10,
                 std::to_underlying(StorageItemRPCFlags::kStorageNone));
             if (!r1.has_value() ||
                 r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Get should return (100, 10)
             auto res1 = handler.get_nnint(key, false);
             if (auto err = check_nnint_get_result(res1, 100, 10); !err) {
               return err;
             }

             // Then add without base (overwrites previous write)
             auto [r2, _gas2] = syscall_NonnegativeIntSetAdd_inner(
                 handler, key, nullptr, 25,
                 std::to_underlying(StorageItemRPCFlags::kStorageNone));
             if (!r2.has_value() ||
                 r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Get should now return (nullopt, 25) - the latest write
             auto res2 = handler.get_nnint(key, false);
             return check_nnint_get_result(res2, std::nullopt, 25);
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);

  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

INSTANTIATE_TEST_SUITE_P(TestDBs, NNIntGetTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
