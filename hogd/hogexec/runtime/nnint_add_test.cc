#include <gtest/gtest.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
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
#include "hogd/storage_item/storage_item_wrapper.h"
#include "storage_item.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class NNIntAddTest : public ::testing::Test,
                     public NativeContractTestFramework {
public:
  NNIntAddTest() : NativeContractTestFramework(false) {}

protected:
  void SetUp() override {}

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

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST_F(NNIntAddTest, ReadWriteAddValue)
{
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("test");
             auto [r, _gas] =
                 syscall_NonnegativeIntSetAdd_inner(handler, key, nullptr, 200);
             if (!r.has_value() || r.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             auto res2 = handler.get_nnint(key, false);
             if (auto err = check_nnint_get_result(res2, std::nullopt, 200);
                 !err) {
               return err;
             }

             auto [r3, _gas3] =
                 syscall_NonnegativeIntSetAdd_inner(handler, key, nullptr, 300);
             if (!r3.has_value() || r3.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // expected result: base 0, add 300
             auto res4 = handler.get_nnint(key, true);
             if (auto err = check_nnint_get_result(res4, 0, 300); !err) {
               return err;
             }

             return {};
           }},
      });

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // mark the contract as existing and deployed within the database
  auto contract_0 = make_native_contract(0);
  in_memory_db_.write_contract(sha256(*contract_0), contract_0);

  auto contract_addr = make_contract_address(0);
  in_memory_db_.write_deployment(contract_addr, sha256(*contract_0));
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
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
      runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);

  advance_block();

  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_addr, "test"));
  ASSERT_TRUE(read_res.has_value());
  ASSERT_EQ(read_res.value().type(), StorageItemType::kItemTypeNonnegativeInt);
  auto nnint_item = read_res.value().nnint();
  ASSERT_EQ(from_xdr(nnint_item), 300);
}

// Test to assert consistency of behavior with regard to the implementation of
// add vs set_add.  Each one overwrites the other.
TEST_F(NNIntAddTest, ReadWriteAddOverwritesSetAdd)
{
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("test");
             int128_t set_value = 200;
             auto [res, _gas] =
                 syscall_NonnegativeIntSetAdd_inner(handler, key, &set_value,
                                                    300);
             if (!res.has_value() || res.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             auto [res2, _gas2] =
                 syscall_NonnegativeIntSetAdd_inner(handler, key, nullptr, 100);
             if (!res2.has_value() || res2.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             auto res3 = handler.get_nnint(key, true);
             if (auto err = check_nnint_get_result(res3, 50, 100); !err) {
               return err;
             }

             return {};
           }},
      });

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  in_memory_db_.write_contract(sha256(*contract_0), contract_0);

  auto contract_addr = make_contract_address(0);
  in_memory_db_.write_deployment(contract_addr, sha256(*contract_0));
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  in_memory_db_.write_item(
      make_contract_data_addrkey(contract_addr, "test"),
      std::make_shared<StorageItemWrapper>(50, get_default_cost_model()));

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
      runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);

  advance_block();

  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_addr, "test"));
  ASSERT_TRUE(read_res.has_value());
  ASSERT_EQ(read_res.value().type(), StorageItemType::kItemTypeNonnegativeInt);
  auto nnint_item = read_res.value().nnint();
  ASSERT_EQ(from_xdr(nnint_item), 150);
}

TEST_F(NNIntAddTest, SetAddWithAdd)
{
  // Create a native contract with two methods.  The first method calls set_add
  // on (0, 200). The second method calls add on (-100). Both methods should
  // assert that the syscalls succeed and then return success.

  // Then, create two transactions.  The first calls the first method,
  // and the second calls the second method.
  // Both transactions should succeed.

  // Then advance the block,
  // and check that the storage item is (100).

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0, // First method: set_add on (0, 200)
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("test");
             int128_t set_value = 0;
             auto [r, _gas] =
                 syscall_NonnegativeIntSetAdd_inner(handler, key, &set_value,
                                                    200);
             if (!r.has_value() || r.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
          {1, // Second method: add on (-100)
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("test");
             auto [r, _gas] = syscall_NonnegativeIntSetAdd_inner(handler, key,
                                                                 nullptr, -100);
             if (!r.has_value() || r.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
      });

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // mark the contract as existing and deployed within the database
  auto contract_0 = make_native_contract(0);
  in_memory_db_.write_contract(sha256(*contract_0), contract_0);

  auto contract_addr = make_contract_address(0);
  in_memory_db_.write_deployment(contract_addr, sha256(*contract_0));
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // First transaction: call method 0 (set_add on (0, 200))
  auto handler1 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      runtime_context);

  auto res1 = handler1.root_invoke();
  ASSERT_TRUE(res1.is_root_success());
  auto commit_res1 = handler1.try_commit_transaction();
  ASSERT_TRUE(commit_res1.has_value());
  ASSERT_TRUE(*commit_res1);

  // Second transaction: call method 1 (add on (-100))
  auto handler2 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(1),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      runtime_context);

  auto res2 = handler2.root_invoke();
  ASSERT_TRUE(res2.is_root_success());
  auto commit_res2 = handler2.try_commit_transaction();
  ASSERT_TRUE(commit_res2.has_value());
  ASSERT_TRUE(*commit_res2);

  advance_block();

  // Check that the storage item is (100)
  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_addr, "test"));
  ASSERT_TRUE(read_res.has_value());
  ASSERT_EQ(read_res.value().type(), StorageItemType::kItemTypeNonnegativeInt);
  auto nnint_item = read_res.value().nnint();
  ASSERT_EQ(from_xdr(nnint_item), 100);

  // Now assert that nnint add conflicts with set_add, if the set value differs
  // from the base value.
  auto handler3 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      runtime_context);

  auto res3 = handler3.root_invoke();
  ASSERT_TRUE(res3.is_root_success());
  auto commit_res3 = handler3.try_commit_transaction();
  ASSERT_TRUE(commit_res3.has_value());
  ASSERT_TRUE(*commit_res3);

  // Fourth transaction: call method 1 (add on (-100))
  auto handler4 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(1),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      runtime_context);

  auto res4 = handler4.root_invoke();
  ASSERT_TRUE(res4.is_root_success());
  auto commit_res4 = handler4.try_commit_transaction();
  ASSERT_TRUE(commit_res4.has_value());
  ASSERT_FALSE(*commit_res4);

  advance_block();

  // Check that the storage item is (100)
  read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_addr, "test"));
  ASSERT_TRUE(read_res.has_value());
  ASSERT_EQ(read_res.value().type(), StorageItemType::kItemTypeNonnegativeInt);
  nnint_item = read_res.value().nnint();
  ASSERT_EQ(from_xdr(nnint_item), 200);
}
// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
