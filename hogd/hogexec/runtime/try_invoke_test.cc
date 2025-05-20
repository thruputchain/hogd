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
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/mockdb/in_memory_database.h"
#include "storage_item.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class TryInvokeTest : public ::testing::Test,
                      public NativeContractTestFramework {
protected:
  void SetUp() override {}
};

// Not really a try invoke test, but it's a convenient way to test general
// invocation.
TEST_F(TryInvokeTest, ReadWriteBytestring)
{ // A native contract with the following methods:
  // 0: takes the input calldata and writes it to a hardcoded storage key.
  // 1: Reads the bytestring from the hardcoded storage key and returns it.
  // 2: Invokes method 0 and then method 1, checking that the written value is
  // read correctly.
  auto native_contract_0 = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<int32_t> {
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

             return 0;
           }},
          {1,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<int32_t> {
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

             handler.set_return_buffer(std::move(out));
             return 0;
           }},
          {2,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<int32_t> {
             auto calldata = handler.get_calldata();

             auto [r0, _g0] = handler.invoke(
                 Invocation{.invoked_address = handler.get_current_address(),
                            .method = 0,
                            .gas_limit = UINT64_MAX,
                            .calldata = calldata},
                 runtime.get_available_gas(),
                 std::to_underlying(InvokeFlag::kInvokeFlagNone));

             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             auto [r1, _g1] = handler.invoke(
                 Invocation{.invoked_address = handler.get_current_address(),
                            .method = 1,
                            .gas_limit = UINT64_MAX,
                            .calldata = {}},
                 runtime.get_available_gas(),
                 std::to_underlying(InvokeFlag::kInvokeFlagNone));

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

             return 0;
           }}});
  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_0));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // mark the contract as existing and deployed within the database
  auto contract_0 = make_native_contract(0);
  in_memory_db_.write_contract(sha256(*contract_0), contract_0);
  in_memory_db_.write_deployment(ContractAddress{}, sha256(*contract_0));
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(ContractAddress{}, 1'000'000'000);

  // now invoke the contract

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation = Invocation{.invoked_address = ContractAddress{},
                                           .method = 2,
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {0, 1, 2, 3, 4, 5}},
              }},
      runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.result_);
  ASSERT_EQ(*res.result_, 0);

  auto r = handler.try_commit_transaction()();
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(*r);
}

TEST_F(TryInvokeTest, TryInvokeGoodWorkflow)
{ // A native contract with the following methods:
  // 0: Invokes method 1, in a try_invoke frame.
  // 1: Makes a bytestring write.
  auto native_contract_0 = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<int32_t> {
             auto const &calldata = handler.get_calldata();
             auto [r0, _g0] = handler.invoke(
                 Invocation{.invoked_address = handler.get_current_address(),
                            .method = 1,
                            .gas_limit = UINT64_MAX,
                            .calldata = calldata},
                 runtime.get_available_gas(),
                 std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));

             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             if (r0.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return 0;
           }},
          {1,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<int32_t> {
             auto key = make_storage_key("test");
             auto res =
                 handler.set_bytestring(key, std::as_bytes(std::span{"hello"}));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }

             return 0;
           }},
      });
  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_0));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // mark the contract as existing and deployed within the database
  auto contract_0 = make_native_contract(0);
  in_memory_db_.write_contract(sha256(*contract_0), contract_0);
  in_memory_db_.write_deployment(ContractAddress{}, sha256(*contract_0));
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(ContractAddress{}, 1'000'000'000);

  // now invoke the contract

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation = Invocation{.invoked_address = ContractAddress{},
                                           .method = 0,
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}},
              }},
      runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.result_);
  ASSERT_EQ(*res.result_, 0);

  auto r = handler.try_commit_transaction()();
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
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  ASSERT_EQ(read_res.value().bytestring(), expect);
}

TEST_F(TryInvokeTest, TryInvokeHandleErrors)
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
              NativeRuntime & /*unused*/) -> InvokeStatus<int32_t> {
             auto key = make_storage_key("test");
             auto res =
                 handler.set_bytestring(key, std::as_bytes(std::span{"hello"}));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return 0;
           }},
          {1,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<int32_t> {
             auto key = make_storage_key("test");
             auto res =
                 handler.set_bytestring(key, std::as_bytes(std::span{"world"}));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return 0;
           }},
          {2,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<int32_t> {
             // Try invoke method 0
             auto [r0, _g0] = handler.invoke(
                 Invocation{.invoked_address = handler.get_current_address(),
                            .method = 0,
                            .gas_limit = UINT64_MAX,
                            .calldata = {}},
                 runtime.get_available_gas(),
                 std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Try invoke method 1
             auto [r1, _g1] = handler.invoke(
                 Invocation{.invoked_address = handler.get_current_address(),
                            .method = 1,
                            .gas_limit = UINT64_MAX,
                            .calldata = {}},
                 runtime.get_available_gas(),
                 std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r1.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return 0;
           }},
          {3,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<int32_t> {
             // Try invoke method 0
             auto [r0, _g0] = handler.invoke(
                 Invocation{.invoked_address = handler.get_current_address(),
                            .method = 0,
                            .gas_limit = UINT64_MAX,
                            .calldata = {}},
                 runtime.get_available_gas(),
                 std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Return an error
             return InvokeError{InvokeErrorCode::kInvokeDeterministic};
           }},
          {4,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<int32_t> {
             // Regular invoke of method 3
             auto [r0, _g0] = handler.invoke(
                 Invocation{.invoked_address = handler.get_current_address(),
                            .method = 3,
                            .gas_limit = UINT64_MAX,
                            .calldata = {}},
                 runtime.get_available_gas(),
                 std::to_underlying(InvokeFlag::kInvokeFlagNone));
             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             // Try invoke method 1
             auto [r1, _g1] = handler.invoke(
                 Invocation{.invoked_address = handler.get_current_address(),
                            .method = 1,
                            .gas_limit = UINT64_MAX,
                            .calldata = {}},
                 runtime.get_available_gas(),
                 std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r1.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return 0;
           }},
      });
  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_0));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // mark the contract as existing and deployed within the database
  auto contract_0 = make_native_contract(0);
  in_memory_db_.write_contract(sha256(*contract_0), contract_0);

  // Test case 1: sequential good try invokes
  // Invoke method 2, starting with a clean contract deployment
  in_memory_db_.write_deployment(make_contract_address(1), sha256(*contract_0));
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
                                   .method = 2,
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
                }},
        runtime_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.result_);
    ASSERT_EQ(*res.result_, 0);
    auto commit_res = handler.try_commit_transaction()();
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
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    ASSERT_EQ(read_res.value().bytestring(), expect);
  }

  // Test case 2: unwinding a frame that contains a try_invoke
  // Invoke method 3, starting with a clean contract deployment
  in_memory_db_.write_deployment(make_contract_address(2), sha256(*contract_0));
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
                                   .method = 3,
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
                }},
        runtime_context);
    auto res = handler.root_invoke();
    ASSERT_FALSE(res.result_); // Should return error
    auto commit_res = handler.try_commit_transaction()();
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
  in_memory_db_.write_deployment(make_contract_address(3), sha256(*contract_0));
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
                                   .method = 4,
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
                }},
        runtime_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.result_);
    ASSERT_EQ(*res.result_, 0);
    auto commit_res = handler.try_commit_transaction()();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
    advance_block();
    // Storage should be updated to "world"
    auto read_res = lookup_storage_item_from_db(
        make_contract_data_addrkey(make_contract_address(3), "test"));
    ASSERT_TRUE(read_res.has_value());
    auto expect_span = std::as_bytes(std::span{"world"});
    Bytestring expect;
    expect.resize(expect_span.size());
    std::memcpy(expect.data(), expect_span.data(), expect_span.size());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    ASSERT_EQ(read_res.value().bytestring(), expect);
  }
}

TEST_F(TryInvokeTest, TryInvokeHandleDBFailure)
{
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
              NativeRuntime & /*unused*/) -> InvokeStatus<int32_t> {
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

             return 0; // Success
           }},
          {1,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<int32_t> {
             auto const &calldata = handler.get_calldata();
             if (calldata.size() != 2) { // Expecting two booleans
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             bool expect_first_succeeds = static_cast<bool>(calldata[0]);
             bool expect_second_succeeds = static_cast<bool>(calldata[1]);

             Calldata args_first_invoke = {10, static_cast<uint8_t>(-5)};
             auto [r0, _g0] = handler.invoke(
                 Invocation{.invoked_address = handler.get_current_address(),
                            .method = 0,
                            .gas_limit = UINT64_MAX,
                            .calldata = args_first_invoke},
                 runtime.get_available_gas(),
                 std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));

             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             bool first_succeeded = (r0.value() == SyscallErrorCode::kNone);
             if (first_succeeded != expect_first_succeeds) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             Calldata args_second_invoke = {5, 10};
             auto [r1, _g1] = handler.invoke(
                 Invocation{.invoked_address = handler.get_current_address(),
                            .method = 0,
                            .gas_limit = UINT64_MAX,
                            .calldata = args_second_invoke},
                 runtime.get_available_gas(),
                 std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));

             if (!r1.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             bool second_succeeded = (r1.value() == SyscallErrorCode::kNone);
             if (second_succeeded != expect_second_succeeds) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return 0;
           }},
          {2,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<int32_t> {
             auto const &calldata = handler.get_calldata();
             if (calldata.size() != 1) { // Expecting one boolean
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             bool expect_invoke_succeeds = static_cast<bool>(calldata[0]);

             Calldata args_invoke = {5, static_cast<uint8_t>(-5)};
             auto [r0, _g0] = handler.invoke(
                 Invocation{.invoked_address = handler.get_current_address(),
                            .method = 0,
                            .gas_limit = UINT64_MAX,
                            .calldata = args_invoke},
                 runtime.get_available_gas(),
                 std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));

             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             bool invoke_succeeded = (r0.value() == SyscallErrorCode::kNone);
             if (invoke_succeeded != expect_invoke_succeeds) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return 0;
           }}});

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_db_failure));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  auto contract_template = make_native_contract(0); // native_id = 0
  in_memory_db_.write_contract(sha256(*contract_template), contract_template);

  // Test case 1:
  // Test that try invoke can acquire an exclusive lock.
  ContractAddress tc1_addr = make_contract_address(30); // Using a new address
  in_memory_db_.write_deployment(tc1_addr, sha256(*contract_template));
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(tc1_addr, 1'000'000'000);

  // TC1.1: First, invoke method 1 with parameters [true, true]. This should
  // succeed. This should leave, in the database, an exclusive lock on the item.
  SystemCallHandler tc1_handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation = Invocation{.invoked_address = tc1_addr,
                                           .method = 1,
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {1, 1}},
              }},
      runtime_context);
  {
    auto res = tc1_handler.root_invoke();
    ASSERT_TRUE(res.result_);
    ASSERT_EQ(*res.result_, 0);
  }

  // TC1.2: Then, in the same block, before committing the first transaction,
  // create a second transaction that invokes method 2 with parameter [false].
  // This should succeed (the try invoke fails, because the first tx still holds
  // the exclusive lock).
  SystemCallHandler tc1_handler2 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation = Invocation{.invoked_address = tc1_addr,
                                           .method = 2,
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {0}},
              }},
      runtime_context);
  {
    auto res = tc1_handler2.root_invoke();
    ASSERT_TRUE(res.result_);
    ASSERT_EQ(*res.result_, 0);
    auto commit_res = tc1_handler2.try_commit_transaction()();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }

  // TC1.3: Then commit the first transaction (releasing the exclusive lock to a
  // set_typeclass lock).
  {
    auto commit_res = tc1_handler.try_commit_transaction()();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }

  // TC1.4: Then invoke method 2 again, with parameter [true]. This should
  // succeed.
  SystemCallHandler tc1_handler3 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation = Invocation{.invoked_address = tc1_addr,
                                           .method = 2,
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {1}},
              }},
      runtime_context);
  {
    auto res = tc1_handler3.root_invoke();
    ASSERT_TRUE(res.result_);
    ASSERT_EQ(*res.result_, 0);
    auto commit_res = tc1_handler3.try_commit_transaction()();
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
  in_memory_db_.write_deployment(tc2_addr, sha256(*contract_template));
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(tc2_addr, 1'000'000'000);

  // Step 1 & 2: Invoke M2({true}) and commit.
  SystemCallHandler tc2_handler1 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation = Invocation{.invoked_address = tc2_addr,
                                           .method = 2,
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {1}}, // true
              }},
      runtime_context);
  {
    auto res = tc2_handler1.root_invoke();
    ASSERT_TRUE(res.result_);
    ASSERT_EQ(*res.result_, 0);
    auto commit_res = tc2_handler1.try_commit_transaction()();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }

  // Step 3 & 4: Invoke M1({false, true}) and commit.
  SystemCallHandler tc2_handler2 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation = Invocation{.invoked_address = tc2_addr,
                                           .method = 1,
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {0, 1}}, // {false, true}
              }},
      runtime_context);
  {
    auto res = tc2_handler2.root_invoke();
    ASSERT_TRUE(res.result_);
    ASSERT_EQ(*res.result_, 0);
    auto commit_res = tc2_handler2.try_commit_transaction()();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }

  advance_block();
}

} // namespace hogd
