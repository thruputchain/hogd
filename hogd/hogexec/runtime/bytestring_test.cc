#include <cassert>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/db_interface/block_manager.h"
#include "hogd/db_interface/db_executor_server.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/mockdb/in_memory_database.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace hogd {

class BytestringTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    constexpr static uint32_t kPortNumber = 10000;

    db_executor_server_.start_server(kPortNumber);

    db_proxy_ =
        std::make_unique<DatabaseProxy>(cp_, LocalMockDB{.read_port_number_ =
                                                             10000});
    [[maybe_unused]] auto res = block_manager_.add_child(0, 1);
    assert(res);
    block_number_++;
  }

  void advance_block()
  {
    auto r = block_manager_.get_block_data(block_number_);
    assert(r);
    r->close_block();

    [[maybe_unused]] auto r2 =
        block_manager_.add_child(block_number_, block_number_ + 1);
    assert(r2);
    block_number_++;
  }

  x23::ThreadPool pool_ = x23::ThreadPool(1);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);

  InMemoryDatabase in_memory_db_ = InMemoryDatabase();
  BlockManager block_manager_ = BlockManager(0, &in_memory_db_);
  DbExecutorServer db_executor_server_ = DbExecutorServer(&block_manager_);

  std::unique_ptr<DatabaseProxy> db_proxy_;
  BlockID block_number_ = 0;
};

namespace {
auto
make_native_contract(uint32_t contract_id)
{
  auto contract = std::make_shared<Contract>(ContractType::kNative);
  contract->native_contract_id() = contract_id;
  return contract;
}

ContractAddress
make_contract_address(uint8_t id)
{
  ContractAddress address{};
  address[0] = id;
  return address;
}
} // namespace

TEST_F(BytestringTest, ReadWriteBytestring)
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
             auto key = StorageKey{};
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
             auto key = StorageKey{};
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

  // now invoke the contract
  StorageSnapshotCache snapshot(1, *db_proxy_);

  auto handler = SystemCallHandler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation = Invocation{.invoked_address = ContractAddress{},
                                           .method = 2,
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {0, 1, 2, 3, 4, 5}},
              }},
      snapshot, PerTransactionDatabaseProxy(*db_proxy_, 1, 0),
      ProposalModeData{}, runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.result_);
  ASSERT_EQ(*res.result_, 0);

  auto r = handler.try_commit_transaction()();
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(*r);
}

TEST_F(BytestringTest, TryInvokeGoodWorkflow)
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
             auto key = StorageKey{};
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

  // now invoke the contract
  StorageSnapshotCache snapshot(1, *db_proxy_);

  auto handler = SystemCallHandler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation = Invocation{.invoked_address = ContractAddress{},
                                           .method = 0,
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}},
              }},
      snapshot, PerTransactionDatabaseProxy(*db_proxy_, 1, 0),
      ProposalModeData{}, runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.result_);
  ASSERT_EQ(*res.result_, 0);

  auto r = handler.try_commit_transaction()();
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(*r);

  advance_block();

  auto read_res =
      db_proxy_->get_snapshot_item(block_number_, AddressAndKey{})();
  ASSERT_TRUE(read_res.has_value());
  auto expect_span = std::as_bytes(std::span{"hello"});
  Bytestring expect;
  expect.resize(expect_span.size());
  std::memcpy(expect.data(), expect_span.data(), expect_span.size());
  ASSERT_EQ(read_res.value().value().bytestring(), expect);
}

TEST_F(BytestringTest, TryInvokeHandleErrors)
{
  // A native contract with the following methods:
  // 0: Performs a bytestring write of "hello".
  // 1: Performs a bytestring write of "world".
  // 2: Invokes method 0, in a try_invoke frame.  Then invokes method 1, in
  // another try_invoke frame. (sequential try invokes) 3: Invokes method 0, in
  // a try_invoke frame.  Then returns an error. 4: Invokes method 3, in a
  // regular frame.  Then invokes method 1, in a try_invoke frame.
  auto native_contract_0 = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<int32_t> {
             auto key = StorageKey{};
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
             auto key = StorageKey{};
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
  {
    StorageSnapshotCache snapshot(block_number_, *db_proxy_);
    auto handler = SystemCallHandler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = make_contract_address(1),
                                   .method = 2,
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
                }},
        snapshot, PerTransactionDatabaseProxy(*db_proxy_, 1, 0),
        ProposalModeData{}, runtime_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.result_);
    ASSERT_EQ(*res.result_, 0);
    auto commit_res = handler.try_commit_transaction()();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);

    advance_block();
    // Check that the last write was "world"
    auto read_res = db_proxy_->get_snapshot_item(
        block_number_, AddressAndKey{.address = make_contract_address(1),
                                     .key = StorageKey{}})();
    ASSERT_TRUE(read_res.has_value());
    ASSERT_TRUE(read_res.value().has_value());
    auto expect_span = std::as_bytes(std::span{"world"});
    Bytestring expect;
    expect.resize(expect_span.size());
    std::memcpy(expect.data(), expect_span.data(), expect_span.size());
    ASSERT_EQ(read_res.value().value().bytestring(), expect);
  }

  // Test case 2: unwinding a frame that contains a try_invoke
  // Invoke method 3, starting with a clean contract deployment
  in_memory_db_.write_deployment(make_contract_address(2), sha256(*contract_0));
  {
    StorageSnapshotCache snapshot(block_number_, *db_proxy_);
    auto handler = SystemCallHandler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = make_contract_address(2),
                                   .method = 3,
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
                }},
        snapshot, PerTransactionDatabaseProxy(*db_proxy_, 2, 0),
        ProposalModeData{}, runtime_context);
    auto res = handler.root_invoke();
    ASSERT_FALSE(res.result_); // Should return error
    auto commit_res = handler.try_commit_transaction()();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_FALSE(*commit_res); // Transaction should not commit
    advance_block();
    // Storage should not be updated
    auto read_res = db_proxy_->get_snapshot_item(
        block_number_, AddressAndKey{.address = make_contract_address(2),
                                     .key = StorageKey{}})();
    // Should be empty or not present
    ASSERT_TRUE(read_res.has_value());
    ASSERT_FALSE(read_res.value().has_value());
  }

  // Test case 3: bad try_invoke followed by a good
  // Invoke method 4, starting with a clean contract deployment
  in_memory_db_.write_deployment(make_contract_address(3), sha256(*contract_0));
  {
    StorageSnapshotCache snapshot(block_number_, *db_proxy_);
    auto handler = SystemCallHandler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = make_contract_address(3),
                                   .method = 4,
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
                }},
        snapshot, PerTransactionDatabaseProxy(*db_proxy_, 3, 0),
        ProposalModeData{}, runtime_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.result_);
    ASSERT_EQ(*res.result_, 0);
    auto commit_res = handler.try_commit_transaction()();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
    advance_block();
    // Storage should be updated to "world"
    auto read_res = db_proxy_->get_snapshot_item(
        block_number_, AddressAndKey{.address = make_contract_address(3),
                                     .key = StorageKey{}})();
    ASSERT_TRUE(read_res.has_value());
    ASSERT_TRUE(read_res.value().has_value());
    auto expect_span = std::as_bytes(std::span{"world"});
    Bytestring expect;
    expect.resize(expect_span.size());
    std::memcpy(expect.data(), expect_span.data(), expect_span.size());
    ASSERT_EQ(read_res.value().value().bytestring(), expect);
  }
}

} // namespace hogd
