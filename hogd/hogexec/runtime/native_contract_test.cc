#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_util/block_snapshot_server.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace hogd {

class NativeContractTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    auto contract_0 = std::make_unique<NativeContract>(
        std::unordered_map<MethodID, NativeContract::function_t>{
            {0,
             [](SystemCallHandler & /*unused*/, NativeRuntime & /*unused*/)
                 -> InvokeStatus<int32_t> { return 0; }},
            {1,
             [](SystemCallHandler & /*unused*/,
                NativeRuntime & /*unused*/) -> InvokeStatus<int32_t> {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }}});

    // Invokes another contract, returns the error code of the other contract
    auto contract_1 = std::make_unique<NativeContract>(
        std::unordered_map<MethodID, NativeContract::function_t>{
            {0,
             [](SystemCallHandler &handler,
                NativeRuntime & /*unused*/) -> InvokeStatus<int32_t> {
               auto const &calldata = handler.get_calldata();

               if (calldata.size() != 36) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
               MethodID invoked_id =
                   *reinterpret_cast<const MethodID *>(calldata.data());
               ContractAddress invoked_addr;
               std::memcpy(invoked_addr.data(), calldata.data() + 4,
                           invoked_addr.size());

               auto invoke_res = handler.invoke(
                   Invocation{.invoked_address = invoked_addr,
                              .method = invoked_id,

                              .gas_limit =
                                  std::numeric_limits<GasAmount>::max(),
                              .calldata = {}

                   },
                   std::numeric_limits<GasAmount>::max());

               if (invoke_res.first) {
                 return std::to_underlying(*invoke_res.first);
               }
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }}});

    std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(contract_0));
    contract_map.emplace(1, std::move(contract_1));

    runtime_context_ = std::make_shared<NativeContext>(std::move(contract_map));
    snapshot_server_.start_server(10000);
    db_proxy_ =
        std::make_unique<DatabaseProxy>(cp_, LocalMockDB{.read_port_number_ =
                                                             10000});
  }

  x23::ThreadPool pool_ = x23::ThreadPool(1);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);

  BlockSnapshotServer snapshot_server_ = BlockSnapshotServer(0);

  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::shared_ptr<RuntimeContext> runtime_context_;
};

TEST_F(NativeContractTest, BasicInstantiate)
{
  StorageSnapshotCache snapshot(0, *db_proxy_);
  SystemCallHandler handler(SignedTransaction{}, snapshot,
                            PerTransactionDatabaseProxy(*db_proxy_, 0, 0),
                            runtime_context_);

  Contract contract(ContractType::kNative);

  auto runtime = runtime_context_->make_new_runtime(contract, &handler, true);

  auto res = runtime->invoke(0, UINT64_MAX);

  ASSERT_TRUE(res.result_);
  ASSERT_EQ(*res.result_, 0);

  res = runtime->invoke(1, UINT64_MAX);

  ASSERT_FALSE(res.result_);
  ASSERT_EQ(res.result_.error(), InvokeErrorCode::kInvokeDeterministic);
}

} // namespace hogd
