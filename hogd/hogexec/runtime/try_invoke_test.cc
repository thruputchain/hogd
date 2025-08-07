#include <gtest/gtest.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/preset_native_token.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "hogd/hogexec/runtime/syscall_types.h"
#include "model_checker/async.h"
#include "model_checker/work_queue.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class TryInvokeTest : public ::testing::TestWithParam<TestDB>,
                      public NativeContractTestFramework {
public:
  TryInvokeTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  void SetUp() override {}
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// Not really a try invoke test, but it's a convenient way to test general
// invocation.
TEST_P(TryInvokeTest, ReadWriteBytestring)
{ // A native contract with the following methods:
  // 0: takes the input calldata and writes it to a hardcoded storage key.
  // 1: Reads the bytestring from the hardcoded storage key and returns it.
  // 2: Invokes method 0 and then method 1, checking that the written value is
  // read correctly.
  auto native_contract_0 = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto const &calldata = handler.get_calldata();
             auto key = make_storage_key("test");
             auto res =
                 handler.set_bytestring(key,
                                        std::as_bytes(std::span(calldata)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }},
          {1,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("test");
             auto res = handler.get_bytestring(key);
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             ReturnBuffer out;
             out.resize(res.value().value().size());
             std::memcpy(out.data(), res.value().value().data(),
                         res.value().value().size());

             handler.set_return_buffer(std::move(out), false);
             return {};
           }},
          {2,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             auto calldata = handler.get_calldata();

             auto [r0, _g0] = handler.invoke(handler.get_current_address(), 0,
                                             runtime.get_available_gas(),
                                             handler.get_calldata(),
                                             std::to_underlying(
                                                 InvokeFlag::kInvokeFlagNone));

             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             auto [r1, _g1] = handler.invoke(
                 handler.get_current_address(), 1, runtime.get_available_gas(),
                 Calldata{}, std::to_underlying(InvokeFlag::kInvokeFlagNone));

             if (!r1.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             auto returned_buffer = handler.get_return_buffer();
             if (!returned_buffer.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             auto const &returned_buffer_data = returned_buffer.value();

             if (returned_buffer_data->size() != calldata.size()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             if (std::memcmp(returned_buffer_data->data(), calldata.data(),
                             calldata.size()) != 0) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }}});
  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_0));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // mark the contract as existing and deployed within the database
  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);
  auto contract_0_addr = make_contract_address(0);
  write_deployment(contract_0_addr, sha256(*contract_0));
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(contract_0_addr, 1'000'000'000);

  // now invoke the contract

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = ContractAddress{},
                                 .entrypoint = entrypoint_from_method_id(2),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {0, 1, 2, 3, 4, 5}},
              }},
      runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction();
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(*r);
}

TEST_P(TryInvokeTest, TryInvokeGoodWorkflow)
{ // A native contract with the following methods:
  // 0: Invokes method 1, in a try_invoke frame.
  // 1: Makes a bytestring write.
  auto native_contract_0 = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             auto [r0, _g0] = handler.invoke(
                 handler.get_current_address(), 1, runtime.get_available_gas(),
                 handler.get_calldata(),
                 std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));

             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             if (r0.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }},
          {1,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("test");
             auto res =
                 handler.set_bytestring(key, std::as_bytes(std::span{"hello"}));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }

             return {};
           }},
      });
  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_0));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // mark the contract as existing and deployed within the database
  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);
  write_deployment(ContractAddress{}, sha256(*contract_0));
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(ContractAddress{}, 1'000'000'000);

  // now invoke the contract

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = ContractAddress{},
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction();
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(*r);

  advance_block();

  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(ContractAddress{}, "test"));
  ASSERT_TRUE(read_res.has_value());
  auto expect_span = std::as_bytes(std::span{"hello"});
  Bytestring expect;
  expect.resize(expect_span.size());
  std::memcpy(expect.data(), expect_span.data(), expect_span.size());
  ASSERT_EQ(read_res.value().bytestring(), expect);
}

TEST_P(TryInvokeTest, TryInvokeHandleErrors)
{
  // A native contract with the following methods:
  // 0: Performs a bytestring write of "hello".
  // 1: Performs a bytestring write of "world".
  // 2: Invokes method 0, in a try_invoke frame.  Then invokes method 1, in
  // another try_invoke frame. (sequential try invokes)
  // 3: Invokes method 0, in a try_invoke frame.  Then returns an error.
  // 4: Invokes method 3, in a regular frame.  Then invokes method 1, in a
  // try_invoke frame.
  auto native_contract_0 = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("test");
             auto res =
                 handler.set_bytestring(key, std::as_bytes(std::span{"hello"}));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return {};
           }},
          {1,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("test");
             auto res =
                 handler.set_bytestring(key, std::as_bytes(std::span{"world"}));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return {};
           }},
          {2,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Try invoke method 0
             auto [r0, _g0] = handler.invoke(
                 handler.get_current_address(), 0, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Try invoke method 1
             auto [r1, _g1] = handler.invoke(
                 handler.get_current_address(), 1, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r1.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
          {3,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Try invoke method 0
             auto [r0, _g0] = handler.invoke(
                 handler.get_current_address(), 0, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Return an error
             return InvokeError{InvokeErrorCode::kInvokeDeterministic};
           }},
          {4,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Regular invoke of method 3
             auto [r0, _g0] = handler.invoke(handler.get_current_address(), 3,
                                             runtime.get_available_gas(), {},
                                             std::to_underlying(
                                                 InvokeFlag::kInvokeFlagNone));
             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Try invoke method 1
             auto [r1, _g1] = handler.invoke(
                 handler.get_current_address(), 1, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r1.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
      });
  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_0));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // mark the contract as existing and deployed within the database
  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);

  // Test case 1: sequential good try invokes
  // Invoke method 2, starting with a clean contract deployment
  write_deployment(make_contract_address(1), sha256(*contract_0));
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(make_contract_address(1), 1'000'000'000);
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = make_contract_address(1),
                                   .entrypoint = entrypoint_from_method_id(2),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}}}},
        runtime_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);

    advance_block();
    // Check that the last write was "world"
    auto read_res = lookup_storage_item_from_db(
        make_contract_data_addrkey(make_contract_address(1), "test"));
    ASSERT_TRUE(read_res.has_value());
    auto expect_span = std::as_bytes(std::span{"world"});
    Bytestring expect;
    expect.resize(expect_span.size());
    std::memcpy(expect.data(), expect_span.data(), expect_span.size());
    ASSERT_EQ(read_res.value().bytestring(), expect);
  }

  // Test case 2: unwinding a frame that contains a try_invoke
  // Invoke method 3, starting with a clean contract deployment
  write_deployment(make_contract_address(2), sha256(*contract_0));
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(make_contract_address(2), 1'000'000'000);
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = make_contract_address(2),
                                   .entrypoint = entrypoint_from_method_id(3),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}}}},
        runtime_context);
    auto res = handler.root_invoke();
    ASSERT_FALSE(res.result_); // Should return error
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_FALSE(*commit_res); // Transaction should not commit
    advance_block();
    // Storage should not be updated
    auto read_res = lookup_storage_item_from_db(
        make_contract_data_addrkey(make_contract_address(2), "test"));
    // Should be empty or not present
    ASSERT_FALSE(read_res.has_value());
  }

  // Test case 3: bad try_invoke followed by a good
  // Invoke method 4, starting with a clean contract deployment
  write_deployment(make_contract_address(3), sha256(*contract_0));
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(make_contract_address(3), 1'000'000'000);
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = make_contract_address(3),
                                   .entrypoint = entrypoint_from_method_id(4),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}}}},
        runtime_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(commit_res.value());
    advance_block();
    // Storage should be updated to "world"
    auto read_res = lookup_storage_item_from_db(
        make_contract_data_addrkey(make_contract_address(3), "test"));
    ASSERT_TRUE(read_res.has_value());
    auto expect_span = std::as_bytes(std::span{"world"});
    Bytestring expect;
    expect.resize(expect_span.size());
    std::memcpy(expect.data(), expect_span.data(), expect_span.size());
    ASSERT_EQ(read_res.value().bytestring(), expect);
  }
}

TEST_P(TryInvokeTest, TryInvokeNNIntAdd)
{
  // Test on try invoke with nnint_add.
  //
  // Native contract with the following methods:
  // 0: Loads the calldata, interprets it as one int8_t value [a], and
  // performs an nnint_add of (a) on a storage key.
  //
  // 1: Invokes method 0 in a try_invoke frame, with arguments [-5].
  // Check that that invocation failed, and then return a deterministic error.
  //
  // The test invokes method 1, and then checks that the transaction fails.
  // Then checks that the storage item is not created.

  // Contract address
  ContractAddress contract_addr = make_contract_address(48);

  // Deploy contract with two methods:
  // 0: Loads calldata as two int8_t values [a, b] and performs nnint_set_add of
  // (a, b) 1: Invokes method 0 in try_invoke frame with [5, -10], checks
  // failure, then returns error
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto const &calldata = handler.get_calldata();
             if (calldata.size() != 1) {
               ADD_FAILURE() << "calldata should have size 1";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             auto key = make_storage_key("test");
             // Interpret calldata bytes as int8_t for set_value and add_value
             auto add_value = static_cast<int8_t>(calldata[0]);

             auto system_res = handler.set_nnint(key, std::nullopt, add_value);

             if (!system_res.has_value()) {
               ADD_FAILURE() << "set_nnint should have succeeded";
               // System-level error during async operation
               return system_res.error().to_invoke_error();
             }

             auto const &syscall_res = system_res.value();
             if (!syscall_res.has_value()) {
               ADD_FAILURE() << "set_nnint should have succeeded";
               // Syscall itself reported an error (e.g., invalid arguments for
               // set_nnint)
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {}; // Success
           }},
          {1,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Invoke method 0 in try_invoke frame with arguments [-5]
             Calldata args = {static_cast<uint8_t>(-5)};
             auto [r0, _g0] = handler.invoke(
                 handler.get_current_address(), 0, runtime.get_available_gas(),
                 args, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));

             if (!r0.has_value()) {
               ADD_FAILURE() << "try_invoke should have failed";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Check that the try_invoke failed (should fail because 5+ -10 is
             // negative)
             if (r0.value() == SyscallErrorCode::kNone) {
               ADD_FAILURE() << "try_invoke should have failed";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Return a deterministic error to make the transaction fail
             return InvokeError{InvokeErrorCode::kInvokeDeterministic};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contract and grant storage tokens
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Invoke method 1 (which should fail)
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
  // The transaction should fail because method 1 returns a deterministic error
  ASSERT_FALSE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  // The transaction should not commit because it failed
  ASSERT_FALSE(*commit_res);

  advance_block();

  // Check that the storage item is not created
  auto data_addrkey = make_contract_data_addrkey(contract_addr, "test");
  auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
  ASSERT_FALSE(data_item_opt.has_value());
}

TEST_P(TryInvokeTest, TryInvokeCrashWhileLocksHeld)
{
  // Test to make sure that the system does not crash when a transaction
  // is aborted while locks are held.
  // Asserts that bug like
  // https://github.com/groundhogchain/hogd/commit/f7a7b0f3737627a42814cea81923322c678df518
  // does not occur again.
  //
  // Native contract with the following methods:
  // 0: Loads the calldata, then perform a bytestring write of that calldata.
  //
  // 1: Invoke method 0 in a try_invoke frame, with argument "hello".
  // Then invokes method 0 again in try_invoke, with argument "world".
  // Both should succeed.
  //
  // 2: Invoke method 0 in a try_invoke frame, with argument "foo".
  // Check that the try_invoke fails.
  // Then return a deterministic error.
  //
  // The test invokes method 1, but does not immediately commit the transaction.
  // Then it starts another transaction that invokes method 2.
  // That second transaction should fail.
  // We then terminate the test without committing either transaction.
  // The test should not crash.

  // Contract address
  ContractAddress contract_addr = make_contract_address(53);

  // Deploy contract with three methods:
  // 0: Write calldata as bytestring to storage
  // 1: Try invoke method 0 twice with "hello" and "world" (both should succeed)
  // 2: Try invoke method 0 with "foo" (should fail due to exclusive lock), then
  // return error
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto const &calldata = handler.get_calldata();
             auto key = make_storage_key("test");
             auto res =
                 handler.set_bytestring(key,
                                        std::as_bytes(std::span(calldata)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
          {1,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Try invoke method 0 with "hello"
             Calldata args_hello = {'h', 'e', 'l', 'l', 'o'};
             auto [r0, _g0] =
                 handler.invoke(handler.get_current_address(), 0,
                                runtime.get_available_gas(), args_hello,
                                std::to_underlying(
                                    InvokeFlag::kInvokeFlagTryInvoke));
             if (!r0.has_value() || r0.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Try invoke method 0 with "world"
             Calldata args_world = {'w', 'o', 'r', 'l', 'd'};
             auto [r1, _g1] =
                 handler.invoke(handler.get_current_address(), 0,
                                runtime.get_available_gas(), args_world,
                                std::to_underlying(
                                    InvokeFlag::kInvokeFlagTryInvoke));
             if (!r1.has_value() || r1.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }},
          {2,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Try invoke method 0 with "foo" (should fail due to exclusive
             // lock)
             Calldata args_foo = {'f', 'o', 'o'};
             auto [r0, _g0] =
                 handler.invoke(handler.get_current_address(), 0,
                                runtime.get_available_gas(), args_foo,
                                std::to_underlying(
                                    InvokeFlag::kInvokeFlagTryInvoke));
             if (!r0.has_value()) {
               ADD_FAILURE() << "shoudl not have system error";
               return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
             }

             // Check that the try_invoke failed (due to exclusive lock)
             if (r0.value() == SyscallErrorCode::kNone) {
               ADD_FAILURE() << "try_invoke should have failed";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Return a deterministic error to make the transaction fail
             return InvokeError{InvokeErrorCode::kInvokeDeterministic};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contract and grant storage tokens
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // First transaction: invoke method 1 (should succeed and acquire exclusive
  // lock)
  auto handler1 = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(1),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);

  auto res1 = handler1.root_invoke();
  ASSERT_TRUE(res1.is_root_success());
  // Don't commit this transaction yet (keeps the exclusive lock)

  // Second transaction: invoke method 2 (should fail due to exclusive lock)
  auto handler2 = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(2),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);

  auto res2 = handler2.root_invoke();
  ASSERT_FALSE(res2.is_root_success()); // Should return error
}

TEST_P(TryInvokeTest, TryInvokeOnExclusiveLock)
{
  // Test on try invoke where a transaction fails after a try_invoke fails.
  //
  // Native contract with the following methods:
  // 0: Loads the calldata, then perform a bytestring write of that calldata.
  //
  // 1: Invoke method 0 in a try_invoke frame, with argument "hello".
  // Then invokes method 0 again in try_invoke, with argument "world".
  // Both should succeed.
  //
  // 2: Invoke method 0 in a try_invoke frame, with argument "foo".
  // Check that the try_invoke fails.
  // Then return a deterministic error.
  //
  // The test invokes method 1, but does not immediately commit the transaction.
  // Then it starts another transaction that invokes method 2.
  // That second transaction should fail.  We then commit the first transaction,
  // which should succeed.

  // Contract address
  ContractAddress contract_addr = make_contract_address(53);

  // Deploy contract with three methods:
  // 0: Write calldata as bytestring to storage
  // 1: Try invoke method 0 twice with "hello" and "world" (both should succeed)
  // 2: Try invoke method 0 with "foo" (should fail due to exclusive lock), then
  // return error
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto const &calldata = handler.get_calldata();
             auto key = make_storage_key("test");
             auto res =
                 handler.set_bytestring(key,
                                        std::as_bytes(std::span(calldata)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
          {1,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Try invoke method 0 with "hello"
             Calldata args_hello = {'h', 'e', 'l', 'l', 'o'};
             auto [r0, _g0] =
                 handler.invoke(handler.get_current_address(), 0,
                                runtime.get_available_gas(), args_hello,
                                std::to_underlying(
                                    InvokeFlag::kInvokeFlagTryInvoke));
             if (!r0.has_value() || r0.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Try invoke method 0 with "world"
             Calldata args_world = {'w', 'o', 'r', 'l', 'd'};
             auto [r1, _g1] =
                 handler.invoke(handler.get_current_address(), 0,
                                runtime.get_available_gas(), args_world,
                                std::to_underlying(
                                    InvokeFlag::kInvokeFlagTryInvoke));
             if (!r1.has_value() || r1.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }},
          {2,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Try invoke method 0 with "foo" (should fail due to exclusive
             // lock)
             Calldata args_foo = {'f', 'o', 'o'};
             auto [r0, _g0] =
                 handler.invoke(handler.get_current_address(), 0,
                                runtime.get_available_gas(), args_foo,
                                std::to_underlying(
                                    InvokeFlag::kInvokeFlagTryInvoke));
             if (!r0.has_value()) {
               ADD_FAILURE() << "shoudl not have system error";
               return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
             }

             // Check that the try_invoke failed (due to exclusive lock)
             if (r0.value() == SyscallErrorCode::kNone) {
               ADD_FAILURE() << "try_invoke should have failed";
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Return a deterministic error to make the transaction fail
             return InvokeError{InvokeErrorCode::kInvokeDeterministic};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contract and grant storage tokens
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // First transaction: invoke method 1 (should succeed and acquire exclusive
  // lock)
  auto handler1 = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(1),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);

  auto res1 = handler1.root_invoke();
  ASSERT_TRUE(res1.is_root_success());
  // Don't commit this transaction yet (keeps the exclusive lock)

  // Second transaction: invoke method 2 (should fail due to exclusive lock)
  auto handler2 = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(2),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);

  auto res2 = handler2.root_invoke();
  ASSERT_FALSE(res2.is_root_success()); // Should return error

  auto commit_res2 = handler2.try_commit_transaction();
  ASSERT_TRUE(commit_res2.has_value());
  ASSERT_FALSE(*commit_res2); // Transaction should not commit

  // Now commit the first transaction (should succeed)
  auto commit_res1 = handler1.try_commit_transaction();
  ASSERT_TRUE(commit_res1.has_value());
  ASSERT_TRUE(*commit_res1);

  advance_block();

  // Check that the storage item was written with "world" (the last write from
  // method 1)
  auto data_addrkey = make_contract_data_addrkey(contract_addr, "test");
  auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
  ASSERT_TRUE(data_item_opt.has_value());

  std::vector<uint8_t> world_data = {'w', 'o', 'r', 'l', 'd'};
  auto expect_span = std::as_bytes(std::span(world_data));
  Bytestring expect;
  expect.resize(expect_span.size());
  std::memcpy(expect.data(), expect_span.data(), expect_span.size());
  ASSERT_EQ(data_item_opt.value().bytestring(), expect);
}

TEST_P(TryInvokeTest, TryInvokeNNIntAddThenAbort)
{
  // Test on try invoke where a transaction fails after a try_invoke fails.
  //
  // Native contract with the following methods:
  // 0: Loads the calldata, interprets it as one int8_t value [a], and
  // performs an nnint_add of (a) on a storage key.
  //
  // 1: Invokes method 0 in a try_invoke frame, with arguments [-5].
  // Check that that invocation failed, and then return a deterministic error.
  //
  // 2: Invokes method 0 with arguments [1].
  // Check that the the invocation succeeds.
  //
  // The text invokes method 2, but does not immediately commit the transaction.
  // Then it starts another transaction that invokes method 1.
  // That second transaction should fail.  We then commit the first transaction,
  // which should succeed.

  // Contract address
  ContractAddress contract_addr = make_contract_address(49);

  // Deploy contract with three methods:
  // 0: Loads calldata as one int8_t value [a] and performs nnint_add of (a)
  // 1: Invokes method 0 in try_invoke frame with [-5], checks failure, then
  // returns error 2: Invokes method 0 with [1], checks success
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto const &calldata = handler.get_calldata();
             if (calldata.size() != 1) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             auto key = make_storage_key("test");
             // Interpret calldata bytes as int8_t for add_value
             auto add_value = static_cast<int8_t>(calldata[0]);

             auto system_res = handler.set_nnint(key, std::nullopt, add_value);

             if (!system_res.has_value()) {
               // System-level error during async operation
               return system_res.error().to_invoke_error();
             }

             auto const &syscall_res = system_res.value();
             if (!syscall_res.has_value()) {
               // Syscall itself reported an error (e.g., invalid arguments for
               // set_nnint)
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {}; // Success
           }},
          {1,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Invoke method 0 in try_invoke frame with arguments [-5]
             Calldata args = {static_cast<uint8_t>(-5)};
             auto [r0, _g0] = handler.invoke(
                 handler.get_current_address(), 0, runtime.get_available_gas(),
                 args, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));

             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Check that the try_invoke failed (should fail because -5 is
             // negative)
             if (r0.value() == SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Return a deterministic error to make the transaction fail
             return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
           }},
          {2,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Invoke method 0 with arguments [1]
             Calldata args = {1};
             auto [r0, _g0] = handler.invoke(
                 handler.get_current_address(), 0, runtime.get_available_gas(),
                 args, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));

             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Check that the invocation succeeds
             if (r0.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contract and grant storage tokens
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // First transaction: invoke method 2 (should succeed)
  auto handler1 = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(2),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);

  auto res1 = handler1.root_invoke();
  ASSERT_TRUE(res1.is_root_success());
  // Don't commit this transaction yet

  // Second transaction: invoke method 1 (should fail)
  auto handler2 = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(1),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);

  auto res2 = handler2.root_invoke();
  ASSERT_FALSE(res2.is_root_success()); // Should return error

  auto commit_res2 = handler2.try_commit_transaction();
  ASSERT_TRUE(commit_res2.has_value());
  ASSERT_FALSE(*commit_res2); // Transaction should not commit

  // Now commit the first transaction (should succeed)
  auto commit_res1 = handler1.try_commit_transaction();
  ASSERT_TRUE(commit_res1.has_value());
  ASSERT_TRUE(*commit_res1);

  advance_block();

  // Check that the storage item was created with value 1 (from method 2)
  auto data_addrkey = make_contract_data_addrkey(contract_addr, "test");
  auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
  ASSERT_TRUE(data_item_opt.has_value());
  ASSERT_EQ(data_item_opt.value().type(),
            StorageItemType::kItemTypeNonnegativeInt);
  EXPECT_EQ(from_xdr(data_item_opt.value().nnint()), 1);
}

TEST_P(TryInvokeTest, TryInvokeHandleDBFailure)
{
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP() << "Pending hogdb exclusive lock release";
    return;
  }
  // Test on try invoke responding to rejection from the database.

  // Native contract with the following methods:
  // 0: Loads the calldata, interprets it as two int8_t values [a, b], and
  // performs an nnint_set_add of (a, b) on a storage key.
  //
  // 1:
  // Load the calldata, and interpret it as two booleans [b1, b2].
  // Invoke method 0 with parameters (10, -5), in a try_invoke frame. Then
  // invoke method 0 with parameters (5, 10), in another try_invoke frame.
  // Assert that b1 is true if and only if the first invocation of method 0
  // succeeded. Assert that b2 is true if and only if the second invocation of
  // method 0 succeeded. Then return success.
  //
  // 2:
  // Load the calldata, and interpret it as a boolean.
  // Invoke method 0 with parameters (5, -5), in a try_invoke frame.
  // Assert that the try_invoke frame returns successfully if and only if the
  // input boolean is true. Then return success.

  auto native_contract_db_failure = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto const &calldata = handler.get_calldata();
             if (calldata.size() != 2) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             auto key = make_storage_key("test");
             // Interpret calldata bytes as int8_t for set_value and add_value
             auto set_value = static_cast<int8_t>(calldata[0]);
             auto add_value = static_cast<int8_t>(calldata[1]);

             auto system_res = handler.set_nnint(key, set_value, add_value);

             if (!system_res.has_value()) {
               // System-level error during async operation
               return system_res.error().to_invoke_error();
             }

             auto const &syscall_res = system_res.value();
             if (!syscall_res.has_value()) {
               // Syscall itself reported an error (e.g., invalid arguments for
               // set_nnint)
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {}; // Success
           }},
          {1,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             auto const &calldata = handler.get_calldata();
             if (calldata.size() != 2) { // Expecting two booleans
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             bool expect_first_succeeds = static_cast<bool>(calldata[0]);
             bool expect_second_succeeds = static_cast<bool>(calldata[1]);

             Calldata args_first_invoke = {10, static_cast<uint8_t>(-5)};
             auto [r0, _g0] =
                 handler.invoke(handler.get_current_address(), 0,
                                runtime.get_available_gas(), args_first_invoke,
                                std::to_underlying(
                                    InvokeFlag::kInvokeFlagTryInvoke));

             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             bool first_succeeded = (r0.value() == SyscallErrorCode::kNone);
             if (first_succeeded != expect_first_succeeds) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             Calldata args_second_invoke = {5, 10};
             auto [r1, _g1] =
                 handler.invoke(handler.get_current_address(), 0,
                                runtime.get_available_gas(), args_second_invoke,
                                std::to_underlying(
                                    InvokeFlag::kInvokeFlagTryInvoke));

             if (!r1.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             bool second_succeeded = (r1.value() == SyscallErrorCode::kNone);
             if (second_succeeded != expect_second_succeeds) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }},
          {2,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             auto const &calldata = handler.get_calldata();
             if (calldata.size() != 1) { // Expecting one boolean
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             bool expect_invoke_succeeds = static_cast<bool>(calldata[0]);

             Calldata args_invoke = {5, static_cast<uint8_t>(-5)};
             auto [r0, _g0] =
                 handler.invoke(handler.get_current_address(), 0,
                                runtime.get_available_gas(), args_invoke,
                                std::to_underlying(
                                    InvokeFlag::kInvokeFlagTryInvoke));

             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             bool invoke_succeeded = (r0.value() == SyscallErrorCode::kNone);
             if (invoke_succeeded != expect_invoke_succeeds) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_db_failure));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  auto contract_template = make_native_contract(0); // native_id = 0
  write_contract(contract_template);

  // Test case 1:
  // Test that try invoke can acquire an exclusive lock.
  ContractAddress tc1_addr = make_contract_address(30); // Using a new address
  write_deployment(tc1_addr, sha256(*contract_template));
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(tc1_addr, 1'000'000'000);

  // TC1.1: First, invoke method 1 with parameters [true, true]. This should
  // succeed. This should leave, in the database, an exclusive lock on the item.
  SystemCallHandler tc1_handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = tc1_addr,
                                 .entrypoint = entrypoint_from_method_id(1),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {1, 1}},
              }},
      runtime_context);
  {
    auto res = tc1_handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
  }

  // TC1.2: Then, in the same block, before committing the first transaction,
  // create a second transaction that invokes method 2 with parameter [false].
  // This should succeed (the try invoke fails, because the first tx still holds
  // the exclusive lock).
  SystemCallHandler tc1_handler2 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = tc1_addr,
                                 .entrypoint = entrypoint_from_method_id(2),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {0}},
              }},
      runtime_context);
  {
    auto res = tc1_handler2.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = tc1_handler2.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }

  // TC1.3: Then commit the first transaction (releasing the exclusive lock to a
  // set_typeclass lock).
  {
    auto commit_res = tc1_handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(commit_res.value());
  }

  // TC1.4: Then invoke method 2 again, with parameter [true]. This should
  // succeed.
  SystemCallHandler tc1_handler3 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = tc1_addr,
                                 .entrypoint = entrypoint_from_method_id(2),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {1}},
              }},
      runtime_context);
  {
    auto res = tc1_handler3.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = tc1_handler3.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }

  advance_block();

  // Test case 2:
  // Test that try invoke can handle a rejection from the database.
  // First, invoke method 2, with parameter [true].  This should succeed.
  // Commit the first transaction.
  // Then, invoke method 1 with parameters [false, true].  This should succeed.
  // Commit the second transaction.
  ContractAddress tc2_addr = make_contract_address(31); // New address for TC2
  write_deployment(tc2_addr, sha256(*contract_template));
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(tc2_addr, 1'000'000'000);

  // Step 1 & 2: Invoke M2({true}) and commit.
  SystemCallHandler tc2_handler1 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = tc2_addr,
                                 .entrypoint = entrypoint_from_method_id(2),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {1}}, // true
              }},
      runtime_context);
  {
    auto res = tc2_handler1.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = tc2_handler1.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(commit_res.value());
  }

  // Step 3 & 4: Invoke M1({false, true}) and commit.
  SystemCallHandler tc2_handler2 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = tc2_addr,
                                 .entrypoint = entrypoint_from_method_id(1),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {0, 1}}, // {false, true}
              }},
      runtime_context);
  {
    auto res = tc2_handler2.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = tc2_handler2.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(commit_res.value());
  }

  advance_block();
}

TEST_P(TryInvokeTest, StorageCostAccounting)
{
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP() << "Pending hogdb storage cost accounting";
    return;
  }
  // Test that checks that storage costs are accounted for correctly in try
  // invoke. A contract has enough storage tokens for one bytestring write. The
  // root frame invokes a try_invoke frame, which attempts to write two
  // bytestrings. That frame fails. The root frame then invokes a try_invoke
  // frame, which makes one bytestring write. That frame succeeds. After commit,
  // the storage token balance should be zero and only the first key should
  // exist.

  // Contract address
  ContractAddress contract_addr = make_contract_address(42);

  // Bytestrings
  auto str_abc = make_bytestring("abc");
  auto str_def = make_bytestring("def");

  auto const *cost_model = get_default_cost_model();

  // Cost for one bytestring write ("abc")
  int128_t one_write_cost = cost_model->kBaseItemCost +
                            cost_model->get_bytestring_cost(str_abc.size());

  // Deploy contract with three methods:
  // 0: Write "abc" to key0
  // 1: Write "abc" to key0 and "def" to key1
  // 2: Root method: try_invoke method 0 (succeeds), then try_invoke method 1
  // (fails)
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [str_abc](SystemCallHandler &handler,
                     NativeRuntime &) -> InvokeStatus<void> {
             auto key0 = make_storage_key("test0");
             auto res =
                 handler.set_bytestring(key0,
                                        std::as_bytes(std::span(str_abc)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
          {1,
           [str_abc, str_def](SystemCallHandler &handler,
                              NativeRuntime &) -> InvokeStatus<void> {
             auto key0 = make_storage_key("test0");
             auto key1 = make_storage_key("test1");
             auto res0 =
                 handler.set_bytestring(key0,
                                        std::as_bytes(std::span(str_abc)));
             if (!res0.has_value() || !res0.value().has_value())
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             auto res1 =
                 handler.set_bytestring(key1,
                                        std::as_bytes(std::span(str_def)));
             if (!res1.has_value() || !res1.value().has_value())
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             return {};
           }},
          {2,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // First, try_invoke method 1 (should fail)
             auto [r1, _g1] = handler.invoke(
                 handler.get_current_address(), 1, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r1.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Should fail (not enough tokens for two writes)
             if (r1.value() == SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Then, try_invoke method 0 (should succeed)
             auto [r0, _g0] = handler.invoke(
                 handler.get_current_address(), 0, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r0.has_value() || r0.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }}});
  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contract and grant exactly enough storage tokens for one write
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, one_write_cost);

  // Single transaction: invoke method 2 (root)
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(2),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(commit_res.value());

  advance_block();

  // 3. Check storage token balance is zero
  AddressAndKey token_addrkey = {
      .address = contract_addr,
      .key = StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};
  auto token_item_opt = lookup_storage_item_from_db(token_addrkey);
  // assert that the free token balance item exists and has value 0
  ASSERT_TRUE(token_item_opt.has_value());
  ASSERT_EQ(token_item_opt.value().type(),
            StorageItemType::kItemTypeNonnegativeInt);
  EXPECT_EQ(from_xdr(token_item_opt.value().nnint()), 0);

  // 4. Check that only key0 exists and has value "abc"
  auto data_addrkey0 = make_contract_data_addrkey(contract_addr, "test0");
  auto data_item0_opt = lookup_storage_item_from_db(data_addrkey0);
  ASSERT_TRUE(data_item0_opt.has_value());
  const StorageItem &data_item0 = data_item0_opt.value();
  ASSERT_EQ(data_item0.bytestring(), str_abc);

  auto data_addrkey1 = make_contract_data_addrkey(contract_addr, "test1");
  auto data_item1_opt = lookup_storage_item_from_db(data_addrkey1);
  ASSERT_FALSE(data_item1_opt.has_value());
}

TEST_P(TryInvokeTest, MarginalCostAccounting)
{
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP() << "Pending hogdb storage cost accounting";
    return;
  }
  // Test to demonstrate necessity for memoizing locked costs.
  // Root frame invokes three try_invoke frames.  Each one writes a bytestring,
  // (on separate keys) but the contract only has enough storage tokens for two
  // and a half. As such, the first and second try_invokes will succeed, and the
  // third will fail.
  //
  // At the end, the storage token balance should be half a bytestring write.

  // Contract address
  ContractAddress contract_addr = make_contract_address(43);

  // Bytestring and cost
  auto str_abc = make_bytestring("abc");
  auto str_def = make_bytestring("def");
  auto str_ghi = make_bytestring("ghi");

  auto const *cost_model = get_default_cost_model();
  int128_t one_write_cost = cost_model->kBaseItemCost +
                            cost_model->get_bytestring_cost(str_abc.size());
  int128_t half_write_cost = one_write_cost / 2;
  int128_t total_tokens = (one_write_cost * 2) + half_write_cost;

  // Deploy contract with four methods:
  // 0: Write "abc" to key0
  // 1: Write "abc" to key1
  // 2: Write "abc" to key2
  // 3: Root method: try_invoke method 0, then 1, then 2
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [str_abc](SystemCallHandler &handler,
                     NativeRuntime &) -> InvokeStatus<void> {
             auto key0 = make_storage_key("test0");
             auto res =
                 handler.set_bytestring(key0,
                                        std::as_bytes(std::span(str_abc)));
             if (!res.has_value())
               return res.error().to_invoke_error();
             if (!res.value().has_value())
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             return {};
           }},
          {1,
           [str_def](SystemCallHandler &handler,
                     NativeRuntime &) -> InvokeStatus<void> {
             auto key1 = make_storage_key("test1");
             auto res =
                 handler.set_bytestring(key1,
                                        std::as_bytes(std::span(str_def)));
             if (!res.has_value())
               return res.error().to_invoke_error();
             if (!res.value().has_value())
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             return {};
           }},
          {2,
           [str_ghi](SystemCallHandler &handler,
                     NativeRuntime &) -> InvokeStatus<void> {
             auto key2 = make_storage_key("test2");
             auto res =
                 handler.set_bytestring(key2,
                                        std::as_bytes(std::span(str_ghi)));
             if (!res.has_value())
               return res.error().to_invoke_error();
             if (!res.value().has_value())
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             return {};
           }},
          {3,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Try invoke method 0
             auto [r0, _g0] = handler.invoke(
                 handler.get_current_address(), 0, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r0.has_value() || r0.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Try invoke method 1
             auto [r1, _g1] = handler.invoke(
                 handler.get_current_address(), 1, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r1.has_value() || r1.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Try invoke method 2 (should fail)
             auto [r2, _g2] = handler.invoke(
                 handler.get_current_address(), 2, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r2.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Should fail (not enough tokens for third write)
             if (r2.value() == SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }}});
  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contract and grant exactly enough storage tokens for two and a half
  // writes
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, total_tokens);

  // Single transaction: invoke method 3 (root)
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(3),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);

  advance_block();

  // Check storage token balance is half a write
  AddressAndKey token_addrkey = {
      .address = contract_addr,
      .key = StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};
  auto token_item_opt = lookup_storage_item_from_db(token_addrkey);
  ASSERT_TRUE(token_item_opt.has_value());
  ASSERT_EQ(token_item_opt.value().type(),
            StorageItemType::kItemTypeNonnegativeInt);
  EXPECT_EQ(from_xdr(token_item_opt.value().nnint()), half_write_cost);

  // Check that key0 and key1 exist and have value "abc" and "def" respectively
  auto data_addrkey0 = make_contract_data_addrkey(contract_addr, "test0");
  auto data_item0_opt = lookup_storage_item_from_db(data_addrkey0);
  ASSERT_TRUE(data_item0_opt.has_value());
  ASSERT_EQ(data_item0_opt.value().bytestring(), str_abc);

  auto data_addrkey1 = make_contract_data_addrkey(contract_addr, "test1");
  auto data_item1_opt = lookup_storage_item_from_db(data_addrkey1);
  ASSERT_TRUE(data_item1_opt.has_value());
  ASSERT_EQ(data_item1_opt.value().bytestring(), str_def);

  // Check that key2 does not exist
  auto data_addrkey2 = make_contract_data_addrkey(contract_addr, "test2");
  auto data_item2_opt = lookup_storage_item_from_db(data_addrkey2);
  ASSERT_FALSE(data_item2_opt.has_value());
}

TEST_P(TryInvokeTest, CostChangeAccounting)
{
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP() << "Pending hogdb storage cost accounting";
    return;
  }
  // Test to check how marginal costs are accounted for when a try_invoke
  // updates, not creates, an item.
  // Root invocation makes three try_invoke invocations.
  // The first two succeed, and the third fails.
  // The first makes a short bytestring write.
  // The second makes a medium bytestring write.
  // The third makes a long bytestring write.
  // The contract has enough storage tokens for the first two writes, but not
  // the third.

  // Contract address
  ContractAddress contract_addr = make_contract_address(44);

  // Bytestrings
  auto str_short = make_bytestring("abc");            // 3 bytes
  auto str_medium = make_bytestring("abcdef");        // 6 bytes
  auto str_long = make_bytestring("abcdefghijklmno"); // 15 bytes

  auto const *cost_model = get_default_cost_model();

  // Cost for each update
  int128_t cost_short = cost_model->kBaseItemCost +
                        cost_model->get_bytestring_cost(str_short.size());
  int128_t cost_medium = cost_model->kBaseItemCost +
                         cost_model->get_bytestring_cost(str_medium.size());

  // Marginal cost for updating from short to medium
  int128_t marginal_medium = cost_medium - cost_short;

  // Grant enough tokens for the first two updates (create + update), but not
  // the third
  int128_t total_tokens = cost_short + marginal_medium;

  // Deploy contract with four methods:
  // 0: Write "abc" to key0
  // 1: Write "abc" to key1
  // 2: Write "abc" to key2
  // 3: Root method: try_invoke method 0, then 1, then 2
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [str_short](SystemCallHandler &handler,
                       NativeRuntime &) -> InvokeStatus<void> {
             auto key0 = make_storage_key("test0");
             auto res =
                 handler.set_bytestring(key0,
                                        std::as_bytes(std::span(str_short)));
             if (!res.has_value())
               return res.error().to_invoke_error();
             if (!res.value().has_value())
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             return {};
           }},
          {1,
           [str_medium](SystemCallHandler &handler,
                        NativeRuntime &) -> InvokeStatus<void> {
             auto key0 = make_storage_key("test0");
             auto res =
                 handler.set_bytestring(key0,
                                        std::as_bytes(std::span(str_medium)));
             if (!res.has_value())
               return res.error().to_invoke_error();
             if (!res.value().has_value())
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             return {};
           }},
          {2,
           [str_long](SystemCallHandler &handler,
                      NativeRuntime &) -> InvokeStatus<void> {
             auto key0 = make_storage_key("test0");
             auto res =
                 handler.set_bytestring(key0,
                                        std::as_bytes(std::span(str_long)));
             if (!res.has_value())
               return res.error().to_invoke_error();
             if (!res.value().has_value())
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             return {};
           }},
          {3,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Try invoke method 0 (short)
             auto [r0, _g0] = handler.invoke(
                 handler.get_current_address(), 0, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r0.has_value() || r0.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Try invoke method 1 (medium)
             auto [r1, _g1] = handler.invoke(
                 handler.get_current_address(), 1, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r1.has_value() || r1.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Try invoke method 2 (long) -- should fail
             auto [r2, _g2] = handler.invoke(
                 handler.get_current_address(), 2, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r2.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (r2.value() == SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }}});
  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contract and grant exactly enough storage tokens for the first two
  // updates
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, total_tokens);

  // Single transaction: invoke method 3 (root)
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(3),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);

  advance_block();

  // Check storage token balance is zero
  AddressAndKey token_addrkey = {
      .address = contract_addr,
      .key = StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};
  auto token_item_opt = lookup_storage_item_from_db(token_addrkey);
  ASSERT_TRUE(token_item_opt.has_value());
  ASSERT_EQ(token_item_opt.value().type(),
            StorageItemType::kItemTypeNonnegativeInt);
  EXPECT_EQ(from_xdr(token_item_opt.value().nnint()), 0);

  // Check that key0 exists and has value "abcdef"
  auto data_addrkey0 = make_contract_data_addrkey(contract_addr, "test0");
  auto data_item0_opt = lookup_storage_item_from_db(data_addrkey0);
  ASSERT_TRUE(data_item0_opt.has_value());
  ASSERT_EQ(data_item0_opt.value().bytestring(), str_medium);
}

TEST_P(TryInvokeTest, CostChangeDecreaseSuccessCase)
{
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP() << "Pending hogdb storage cost accounting";
    return;
  }
  // Test to make sure that if the cost of an item decreases, the storage token
  // balance is calculated correctly. Root invocation makes two try_invoke
  // invocations. Both succeed. The first makes a long bytestring write. The
  // second makes a short bytestring write. The contract has enough storage
  // tokens for both writes. The end of the test asserts that the storage token
  // balance was charged for the short write.

  // Contract address
  ContractAddress contract_addr = make_contract_address(45);

  // Bytestrings
  auto str_long = make_bytestring("abcdefghijklmno"); // 15 bytes
  auto str_short = make_bytestring("abc");            // 3 bytes

  auto const *cost_model = get_default_cost_model();

  // Costs
  int128_t cost_long = cost_model->kBaseItemCost +
                       cost_model->get_bytestring_cost(str_long.size());
  int128_t cost_short = cost_model->kBaseItemCost +
                        cost_model->get_bytestring_cost(str_short.size());

  // Grant enough tokens for the long write.
  int128_t total_tokens = cost_long;

  // Deploy contract with three methods:
  // 0: Write long string
  // 1: Write short string
  // 2: Root method: try_invoke method 0, then try_invoke method 1
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [str_long](SystemCallHandler &handler,
                      NativeRuntime &) -> InvokeStatus<void> {
             auto key0 = make_storage_key("test0");
             auto res =
                 handler.set_bytestring(key0,
                                        std::as_bytes(std::span(str_long)));
             if (!res.has_value() || !res.value().has_value())
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             return {};
           }},
          {1,
           [str_short](SystemCallHandler &handler,
                       NativeRuntime &) -> InvokeStatus<void> {
             auto key0 = make_storage_key("test0");
             auto res =
                 handler.set_bytestring(key0,
                                        std::as_bytes(std::span(str_short)));
             if (!res.has_value() || !res.value().has_value())
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             return {};
           }},
          {2,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Try invoke method 0 (long)
             auto [r0, _g0] = handler.invoke(
                 handler.get_current_address(), 0, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r0.has_value() || r0.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Try invoke method 1 (short)
             auto [r1, _g1] = handler.invoke(
                 handler.get_current_address(), 1, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r1.has_value() || r1.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }}});
  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contract and grant enough storage tokens for the long write
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, total_tokens);

  // Single transaction: invoke method 2 (root)
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(2),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);

  advance_block();

  // Check storage token balance
  AddressAndKey token_addrkey = {
      .address = contract_addr,
      .key = StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};
  auto token_item_opt = lookup_storage_item_from_db(token_addrkey);
  ASSERT_TRUE(token_item_opt.has_value());
  ASSERT_EQ(token_item_opt.value().type(),
            StorageItemType::kItemTypeNonnegativeInt);
  // Initial tokens was cost_long. Final cost is cost_short.
  // The balance should be total_tokens - cost_short.
  int128_t expected_balance = total_tokens - cost_short;
  EXPECT_EQ(from_xdr(token_item_opt.value().nnint()), expected_balance);

  // Check that key0 exists and has value from the short write.
  auto data_addrkey0 = make_contract_data_addrkey(contract_addr, "test0");
  auto data_item0_opt = lookup_storage_item_from_db(data_addrkey0);
  ASSERT_TRUE(data_item0_opt.has_value());
  ASSERT_EQ(data_item0_opt.value().bytestring(), str_short);
}

TEST_P(TryInvokeTest, CostChangeDecreaseFailCase)
{
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP() << "Pending hogdb storage cost accounting";
    return;
  }
  // Test to make sure that if the cost of an item decreases, the storage token
  // balance is calculated correctly.
  //  Root invocation first makes a try_invocation call into a method that
  // just makes a long bytestring write.
  // Then, it makes a regular invocation into another method.
  // That method makes a try_invoke call into another method, which makes a
  // short bytestring write (that should succeed). The regular invocation then
  // returns an error. The root method should expect that error (and finish
  // successfully).

  // Contract address
  ContractAddress contract_addr = make_contract_address(46);

  // Bytestrings
  auto str_long = make_bytestring("abcdefghijklmno"); // 15 bytes
  auto str_short = make_bytestring("abc");            // 3 bytes

  auto const *cost_model = get_default_cost_model();

  // Costs
  int128_t cost_long = cost_model->kBaseItemCost +
                       cost_model->get_bytestring_cost(str_long.size());

  // Grant enough tokens for the long write.
  int128_t total_tokens = cost_long;

  // Deploy contract with four methods:
  // 0: Write long string
  // 1: Write short string
  // 2: Try-invoke method 1, then return error
  // 3: Root method: try_invoke method 0, then try_invoke method 2
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [str_long](SystemCallHandler &handler,
                      NativeRuntime &) -> InvokeStatus<void> {
             auto key0 = make_storage_key("test0");
             auto res =
                 handler.set_bytestring(key0,
                                        std::as_bytes(std::span(str_long)));
             if (!res.has_value() || !res.value().has_value())
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             return {};
           }},
          {1,
           [str_short](SystemCallHandler &handler,
                       NativeRuntime &) -> InvokeStatus<void> {
             auto key0 = make_storage_key("test0");
             auto res =
                 handler.set_bytestring(key0,
                                        std::as_bytes(std::span(str_short)));
             if (!res.has_value() || !res.value().has_value())
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             return {};
           }},
          {2,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Try invoke method 1 (short write)
             auto [r1, _g1] = handler.invoke(
                 handler.get_current_address(), 1, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r1.has_value() || r1.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Now, return an error. This will cause this frame's effects to
             // be rolled back.
             return InvokeError{InvokeErrorCode::kInvokeDeterministic};
           }},
          {3,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Try invoke method 0 (long write)
             auto [r0, _g0] = handler.invoke(
                 handler.get_current_address(), 0, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r0.has_value() || r0.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Regular invoke on method 2
             auto [r2, _g2] = handler.invoke(handler.get_current_address(), 2,
                                             runtime.get_available_gas(), {},
                                             std::to_underlying(
                                                 InvokeFlag::kInvokeFlagNone));
             if (!r2.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // We expect method 2 to fail.
             if (r2.value() == SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }}});
  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contract and grant enough storage tokens for the long write
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, total_tokens);

  // Single transaction: invoke method 3 (root)
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(3),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);

  advance_block();

  // Check storage token balance
  AddressAndKey token_addrkey = {
      .address = contract_addr,
      .key = StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};
  auto token_item_opt = lookup_storage_item_from_db(token_addrkey);
  ASSERT_TRUE(token_item_opt.has_value());
  ASSERT_EQ(token_item_opt.value().type(),
            StorageItemType::kItemTypeNonnegativeInt);
  // The cost of the short write was rolled back.
  // Final cost is cost_long. Initial tokens was cost_long.
  // So the balance should be 0.
  EXPECT_EQ(from_xdr(token_item_opt.value().nnint()), 0);

  // Check that key0 exists and has value from the long write.
  auto data_addrkey0 = make_contract_data_addrkey(contract_addr, "test0");
  auto data_item0_opt = lookup_storage_item_from_db(data_addrkey0);
  ASSERT_TRUE(data_item0_opt.has_value());
  ASSERT_EQ(data_item0_opt.value().bytestring(), str_long);
}

TEST_P(TryInvokeTest, TransferAndUseStorageWithinTransaction)
{
  if (GetParam() == TestDB::kHogDB) {
    GTEST_SKIP() << "Pending hogdb storage cost accounting";
    return;
  }
  // Test to make sure that if a contract transfers storage tokens to another
  // contract, the new storage tokens can be used at the receiver contract
  // within the same transaction.

  // Test creates two NativeContracts, and deploys them at different addresses.
  // The first contract transfers storage tokens to the second contract.
  // The second contract then uses the storage tokens to write a bytestring.

  // This requires building two native contracts, and then deploying them at
  // separate addresses. Transfers should be done by invoking the
  // preset_native_token contract.

  constexpr uint32_t kSenderNativeId = 0;
  constexpr uint32_t kReceiverNativeId = 1;
  constexpr uint32_t kTokenNativeId = 2;

  ContractAddress sender_addr = make_contract_address(50);
  ContractAddress receiver_addr = make_contract_address(51);
  ContractAddress token_contract_addr = make_contract_address(52);

  auto const *cost_model = get_default_cost_model();
  auto str_hello = make_bytestring("hello");
  int128_t write_cost = cost_model->kBaseItemCost +
                        cost_model->get_bytestring_cost(str_hello.size());

  // Sender contract
  auto sender_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [token_contract_addr, receiver_addr,
            write_cost](SystemCallHandler &handler,
                        NativeRuntime &runtime) -> InvokeStatus<void> {
             auto native_token_client =
                 ContractTraitClient<NativeTokenTrait>(token_contract_addr);

             auto [r_transfer, g_transfer] =
                 native_token_client.invoke<"transfer">(handler, receiver_addr,
                                                        to_xdr(write_cost));

             if (!r_transfer.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Invoke receiver contract to use the tokens
             auto [r_receive, g_receive] = handler.invoke(
                 receiver_addr, 0, runtime.get_available_gas(), {},
                 std::to_underlying(InvokeFlag::kInvokeFlagNone));

             if (!r_receive.has_value() ||
                 r_receive.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             auto [r_balance, g_balance] =
                 native_token_client.invoke<"get_balance">(handler);
             if ((!r_balance.has_value()) || !r_balance.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             if (from_xdr(r_balance.value().value()) != 0) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }}});

  // Receiver contract
  auto receiver_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [str_hello](SystemCallHandler &handler,
                       NativeRuntime &) -> InvokeStatus<void> {
             auto key = make_storage_key("test");
             auto res =
                 handler.set_bytestring(key,
                                        std::as_bytes(std::span(str_hello)));
             if (!res.has_value() || !res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }}});

  // Token contract
  auto token_contract = make_native_token_contract();

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(kReceiverNativeId, std::move(receiver_contract));
  contract_map.emplace(kTokenNativeId, std::move(token_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contracts
  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(sender_addr, sha256(*sender_contract_xdr));

  auto receiver_contract_xdr = make_native_contract(kReceiverNativeId);
  write_contract(receiver_contract_xdr);
  write_deployment(receiver_addr, sha256(*receiver_contract_xdr));

  auto token_contract_xdr = make_native_contract(kTokenNativeId);
  write_contract(token_contract_xdr);
  write_deployment(token_contract_addr, sha256(*token_contract_xdr));

  // Grant initial tokens to sender
  setup_grant_storage_token(sender_addr, write_cost);

  // Invoke the sender contract
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = sender_addr,
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

  // Check sender token balance
  auto sender_token_addrkey =
      AddressAndKey{.address = sender_addr,
                    .key = StorageKey(
                        StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};
  auto sender_token_item_opt =
      lookup_storage_item_from_db(sender_token_addrkey);
  ASSERT_TRUE(sender_token_item_opt.has_value());
  EXPECT_EQ(from_xdr(sender_token_item_opt.value().nnint()), 0);

  // Check receiver token balance
  auto receiver_token_addrkey =
      AddressAndKey{.address = receiver_addr,
                    .key = StorageKey(
                        StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};
  auto receiver_token_item_opt =
      lookup_storage_item_from_db(receiver_token_addrkey);
  ASSERT_TRUE(receiver_token_item_opt.has_value());
  EXPECT_EQ(from_xdr(receiver_token_item_opt.value().nnint()), 0);

  // Check receiver storage
  auto data_addrkey = make_contract_data_addrkey(receiver_addr, "test");
  auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
  ASSERT_TRUE(data_item_opt.has_value());
  ASSERT_EQ(data_item_opt.value().bytestring(), str_hello);
}

TEST_P(TryInvokeTest, NNIntAddSimpleInterleavings)
{
  NativeContractBuilder builder;

  StorageKeyContractData key = make_storage_key("test");

  // perform a set_add_nnint
  builder.add(0,
              [&](SystemCallHandler &handler, int64_t set_amount,
                  int64_t add_amount) -> InvokeStatus<void> {
                int128_t set_128 = set_amount;
                auto [r, _gas] =
                    syscall_NonnegativeIntSetAdd_inner(handler, key, &set_128,
                                                       add_amount);

                if (!r.has_value() || r.value() != SyscallErrorCode::kNone) {
                  return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                }
                return {};
              });

  // perform a add_nnint
  builder.add(1,
              [&](SystemCallHandler &handler,
                  int64_t add_amount) -> InvokeStatus<void> {
                auto [r, _gas] =
                    syscall_NonnegativeIntSetAdd_inner(handler, key, nullptr,
                                                       add_amount);
                if (!r.has_value() || r.value() != SyscallErrorCode::kNone) {
                  return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                }
                return {};
              });

  builder.add(
      2,
      [&](SystemCallHandler &handler,
          NativeRuntime &runtime) -> InvokeStatus<void> {
        // perform an add_nnint, then perform a set_add_nnint with the
        // same set value in a try_invoke frame
        auto calldata0 =
            Serializer<NativeContractBuilder::kDefaultSerializationFormat>::
                serialize<int64_t>(100)
                    .value();

        auto [r0, _g0] = handler.invoke(handler.get_current_address(), 1,
                                        runtime.get_available_gas(), calldata0,
                                        std::to_underlying(
                                            InvokeFlag::kInvokeFlagTryInvoke));
        if (!r0.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        // tolerate try_invoke failure

        auto calldata1 =
            Serializer<NativeContractBuilder::kDefaultSerializationFormat>::
                serialize<int64_t, int64_t>(100, -100)
                    .value();

        auto [r1, _g1] = handler.invoke(handler.get_current_address(), 0,
                                        runtime.get_available_gas(), calldata1,
                                        std::to_underlying(
                                            InvokeFlag::kInvokeFlagTryInvoke));
        if (!r1.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        // tolerate try_invoke failure

        return {};
      });
  builder.add(3, [&](SystemCallHandler &handler) -> InvokeStatus<void> {
    // reset contract state
    int128_t set_value = 35;
    auto [r, _gas] =
        syscall_NonnegativeIntSetAdd_inner(handler, key, &set_value, 0);
    // auto r = handler.set_nnint(key, 35, 0);
    // if (!r.has_value() || !r.value().has_value()) {
    if (!r.has_value() || r.value() != SyscallErrorCode::kNone) {
      return InvokeError{InvokeErrorCode::kInvokeDeterministic};
    }
    return {};
  });

  auto native_contract = builder.build();

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr uint32_t kTestContractId = 0;
  contract_map.emplace(kTestContractId, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contracts
  auto test_contract_xdr = make_native_contract(kTestContractId);
  write_contract(test_contract_xdr);
  write_deployment(ContractAddress{}, sha256(*test_contract_xdr));

  // Grant initial tokens to test contract
  setup_grant_storage_token(ContractAddress{}, 1'000'000'000);

  // write an initial int128 value
  StorageKey wrapped_key(StorageKeyType::kStorageKeyTypeContractData);
  wrapped_key.contract_data_key() = key;

  // too much state here to parallelize the experiment
  model::WorkQueue work_queue;

  while (!work_queue.done()) {
    model::RunnableActionSet actions(work_queue);

    actions.add_action(
        [](model::RunnableActionSet &set, auto self,
           auto &runtime_context) -> model::Async {
          co_await set.bg();

          auto handler = self->make_new_system_call_handler(
              SignedTransaction{
                  .tx =
                      Transaction{
                          .invocation =
                              Invocation{.invoked_address = ContractAddress{},
                                         .entrypoint =
                                             entrypoint_from_method_id(2),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}}}},
              runtime_context);
          auto res = handler.root_invoke();
          if (!res.is_root_success()) {
            throw std::runtime_error("root invoke 0 failed");
          }
          co_await set.bg();
          auto commit_res = handler.try_commit_transaction();
          if (!commit_res.has_value()) {
            throw std::runtime_error("try_commit_transaction error");
          }
          co_return;
        },
        this, runtime_context);

    actions.add_action(
        [](model::RunnableActionSet &set, auto self,
           auto &runtime_context) -> model::Async {
          co_await set.bg();

          // If this one preceeds the set_add_nnint in method 2, then that will
          // succeed.
          auto calldata =
              Serializer<NativeContractBuilder::kDefaultSerializationFormat>::
                  serialize<int64_t, int64_t>(100, 100)
                      .value();
          auto handler = self->make_new_system_call_handler(
              SignedTransaction{
                  .tx =
                      Transaction{
                          .invocation =
                              Invocation{.invoked_address = ContractAddress{},
                                         .entrypoint =
                                             entrypoint_from_method_id(0),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = calldata}}},
              runtime_context);
          auto res = handler.root_invoke();
          co_await set.bg();
          if (!res.is_root_success()) {
            co_return;
          }
          auto commit_res = handler.try_commit_transaction();
          if (!commit_res.has_value()) {
            throw std::runtime_error("try_commit_transaction error");
          }
          co_return;
        },
        this, runtime_context);

    {
      auto handler = make_new_system_call_handler(
          SignedTransaction{
              .tx =
                  Transaction{
                      .invocation =
                          Invocation{.invoked_address = ContractAddress{},
                                     .entrypoint = entrypoint_from_method_id(3),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}}}},
          runtime_context);

      auto res = handler.root_invoke();
      ASSERT_TRUE(res.is_root_success());
      auto commit_res = handler.try_commit_transaction();
      ASSERT_TRUE(commit_res.has_value());
      ASSERT_TRUE(*commit_res);
    }

    advance_block();

    ASSERT_EQ(actions.run(), model::ActionResult::kOk);

    advance_block();

    auto res = lookup_storage_item_from_db(
        AddressAndKey{.address = ContractAddress{}, .key = wrapped_key});
    ASSERT_TRUE(res.has_value());
    ASSERT_EQ(res.value().type(), StorageItemType::kItemTypeNonnegativeInt);
    int128_t value = from_xdr(res.value().nnint());

    EXPECT_TRUE(value == 100 || value == 0);

    work_queue.advance_cursor();
  }
}

TEST_P(TryInvokeTest, InvokeErrorHandling)
{
  // This test is a guard against an unchecked error condition in the
  // invoke syscall handler (fixing an earlier bug, PR #200).
  //
  // There are many ways that an invocation can fail.  This test makes sure
  // those errors are passed back to the caller correctly.

  // This test creates two native contracts.  The first has two methods.
  // The first method returns a deterministic error and consumes 100 gas.
  // The second method returns success.
  // The second contract has one method.
  // These are both deployed to separate addresses.
  // This second contract's method performs the following actions:
  // 1. try_invoke on a third address, which has no contract deployed there.
  //    The try_invoke should fail, and the error should be checked as the
  //    appropriate error code.
  // 2. try_invoke on the first contract, but with a method number that does not
  // exist.
  //    The try_invoke should fail, and the error should be checked as the
  //    appropriate error code.
  // 3. try_invoke on the first contract, with the correct method number, but
  // with a gas limit of 50.
  //    The try_invoke should fail, and the error should be checked as the
  //    appropriate error code.
  // 4. try_invoke on the first contract, with the correct method number, but
  // with a gas limit of 1000.
  //    The try_invoke should succeed, and the error should be checked as the
  //    appropriate error code.
  // 5. try_invoke on the first contract, on the method that returns success.
  // Should succeed.

  constexpr uint32_t kErrorContractId = 0;
  constexpr uint32_t kCallerContractId = 1;
  ContractAddress error_contract_addr = make_contract_address(10);
  ContractAddress caller_contract_addr = make_contract_address(11);
  ContractAddress missing_contract_addr = make_contract_address(12);

  // Contract 0: always errors, consumes 100 gas
  auto error_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler & /*handler*/,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             if (!runtime.consume_gas(100)) {
               return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
             }
             return InvokeError{InvokeErrorCode::kInvokeDeterministic};
           }},
          {1,
           [](SystemCallHandler & /*handler*/, NativeRuntime & /*unused*/)
               -> InvokeStatus<void> { return {}; }}});

  // Contract 1: performs the four try_invoke error cases
  auto caller_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [=](SystemCallHandler &handler,
               NativeRuntime &runtime) -> InvokeStatus<void> {
             RuntimeMemoryLength output_return_buffer_len;
             WrappedGasAmount remaining_gas = {runtime.get_available_gas()};
             Calldata calldata = {};
             std::span<const std::byte> calldata_span =
                 std::as_bytes(std::span(calldata.data(), calldata.size()));
             // 1. try_invoke on a missing contract
             {
               auto [res, _gas] = syscall_Invoke_inner(
                   handler, remaining_gas, missing_contract_addr, 0, 1000,
                   calldata_span,
                   std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke),
                   output_return_buffer_len);

               if (!res.has_value() ||
                   res.value() != SyscallErrorCode::kContractLookupFailed) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
             }
             // 2. try_invoke on error_contract, but with a method that does not
             // exist
             {
               auto [res, _gas] = syscall_Invoke_inner(
                   handler, remaining_gas, error_contract_addr, 999, 1000,
                   calldata_span,
                   std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke),
                   output_return_buffer_len);
               if (!res.has_value() ||
                   res.value() != SyscallErrorCode::kDeterministicWasm) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
             }
             // 3. try_invoke on error_contract, correct method, but with gas
             // limit 50 (not enough)
             {
               auto [res, gas] = syscall_Invoke_inner(
                   handler, remaining_gas, error_contract_addr, 0, 50,
                   calldata_span,
                   std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke),
                   output_return_buffer_len);
               // This should return as a deterministic error (handleable), with
               // 50 gas consumed
               if (!res.has_value() ||
                   res.value() != SyscallErrorCode::kDeterministicWasm) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
               if (gas != 50) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
             }
             // 4. try_invoke on error_contract, correct method, with gas limit
             // 1000 (enough)
             {
               auto [res, _gas] = syscall_Invoke_inner(
                   handler, remaining_gas, error_contract_addr, 0, 1000,
                   calldata_span,
                   std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke),
                   output_return_buffer_len);
               if (!res.has_value() ||
                   res.value() != SyscallErrorCode::kDeterministicWasm) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
             }
             // 5. try_invoke on error_contract, valid method, with gas limit
             // 1000 (enough)
             {
               auto [res, _gas] = syscall_Invoke_inner(
                   handler, remaining_gas, error_contract_addr, 1, 1000,
                   calldata_span,
                   std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke),
                   output_return_buffer_len);
               if (!res.has_value() || res.value() != SyscallErrorCode::kNone) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
             }

             return {};
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(kErrorContractId, std::move(error_contract));
  contract_map.emplace(kCallerContractId, std::move(caller_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contracts
  auto error_contract_xdr = make_native_contract(kErrorContractId);
  write_contract(error_contract_xdr);
  write_deployment(error_contract_addr, sha256(*error_contract_xdr));

  auto caller_contract_xdr = make_native_contract(kCallerContractId);
  write_contract(caller_contract_xdr);
  write_deployment(caller_contract_addr, sha256(*caller_contract_xdr));

  // Grant tokens to both contracts
  setup_grant_storage_token(error_contract_addr, 1'000'000'000);
  setup_grant_storage_token(caller_contract_addr, 1'000'000'000);

  // Now invoke the caller contract
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = caller_contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}}}},
      runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);
}

TEST_P(TryInvokeTest, ManualRevertUndoesTryInvokeLocks)
{
  // Assert that if a try_invoke frame successfully acquires locks but then its
  // parent frame does a manual revert, then the locks are released. This test
  // creates one native contract with three methods. The first method is a
  // try_invoke on the second method. The second method does a try_invoke on the
  // third method, and then manually reverts. The third method performs a
  // bytestring write, and then return success. This test invokes the first
  // method.  Afterwards, it asserts that the bytestring was not written.

  // Contract address
  ContractAddress contract_addr = make_contract_address(47);

  // Bytestring to write
  auto str_hello = make_bytestring("hello");
  auto key = make_storage_key("test");

  // Deploy contract with three methods:
  // 0: try_invoke method 1
  // 1: try_invoke method 2, then manually revert
  // 2: Write "hello" to storage and return success
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [key](SystemCallHandler &handler,
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
             if (r1.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             auto res = handler.get_bytestring(key);
             if (!res.has_value()) {
               // assert no system error
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (res.value().has_value()) {
               // assert no bytestring returned
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // assert that we can actually get a ReturnBuffer back
             auto return_buf = handler.get_return_buffer();
             if (!return_buf.has_value() || !return_buf.value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             if (!return_buf.value()->empty()) {
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
             // acquired by method 2
             handler.set_return_buffer(ReturnBuffer{}, true);
             return {};
           }},
          {2,
           [str_hello, key](SystemCallHandler &handler,
                            NativeRuntime &) -> InvokeStatus<void> {
             auto res =
                 handler.set_bytestring(key,
                                        std::as_bytes(std::span(str_hello)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (!res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }}});
  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
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
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);

  advance_block();

  // Check that the bytestring was NOT written (locks were released due to
  // manual revert)
  auto data_addrkey = make_contract_data_addrkey(contract_addr, "test");
  auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
  ASSERT_FALSE(data_item_opt.has_value());
}
// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, TryInvokeTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
