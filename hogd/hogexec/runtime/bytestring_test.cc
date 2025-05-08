#include <cassert>
#include <cstdint>
#include <gtest/gtest.h>

#include <memory>
#include <print>
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
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/mockdb/in_memory_database.h"
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
  }

  x23::ThreadPool pool_ = x23::ThreadPool(1);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);

  InMemoryDatabase in_memory_db_ = InMemoryDatabase();
  BlockManager block_manager_ = BlockManager(0, &in_memory_db_);
  DbExecutorServer db_executor_server_ = DbExecutorServer(&block_manager_);

  std::unique_ptr<DatabaseProxy> db_proxy_;
};

namespace {
auto
make_native_contract(uint32_t contract_id)
{
  auto contract = std::make_shared<Contract>(ContractType::kNative);
  contract->native_contract_id() = contract_id;
  return contract;
}
} // namespace

TEST_F(BytestringTest, ReadBytestring)
{
  // A native contract with the following methods:
  // 0: takes the input calldata and writes it to a hardcoded storage key.
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
                                           .method = 0,
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {0, 1, 2, 3, 4, 5}},
              }},
      snapshot, runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.result_);
  ASSERT_EQ(*res.result_, 0);
}

} // namespace hogd
