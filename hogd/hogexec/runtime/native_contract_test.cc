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
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
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

               if (calldata.size() !=
                   sizeof(MethodID) + sizeof(ContractAddress)) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
               MethodID invoked_id =
                   *reinterpret_cast<const MethodID *>(calldata.data());
               ContractAddress invoked_addr;
               std::memcpy(invoked_addr.data(),
                           calldata.data() + sizeof(MethodID),
                           invoked_addr.size());

               auto invoke_res = handler.invoke(
                   Invocation{.invoked_address = invoked_addr,
                              .method = invoked_id,

                              .gas_limit =
                                  std::numeric_limits<GasAmount>::max(),
                              .calldata = {}

                   },
                   std::numeric_limits<GasAmount>::max(),
                   std::to_underlying(InvokeFlag::kInvokeFlagNone));

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
                            ProposalModeData{}, runtime_context_);

  Contract contract(ContractType::kNative);
  contract.native_contract_id() = 0;

  auto runtime = runtime_context_->make_new_runtime(contract, &handler, true);

  auto res = runtime->invoke(0, UINT64_MAX);

  ASSERT_TRUE(res.result_);
  ASSERT_EQ(*res.result_, 0);

  res = runtime->invoke(1, UINT64_MAX);

  ASSERT_FALSE(res.result_);
  ASSERT_EQ(res.result_.error(), InvokeErrorCode::kInvokeDeterministic);
}

TEST_F(NativeContractTest, GasCostsAccumulate)
{
  NativeContractBuilder builder;
  ContractAddress address{};

  builder.add(0,
              [](NativeRuntime &runtime,
                 uint32_t test_amount) -> InvokeStatus<int32_t> {
                if (!runtime.consume_gas(test_amount)) {
                  return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
                }
                return 0;
              });
  builder.add(
      1,
      [&address](SystemCallHandler &handler,
                 NativeRuntime &runtime) -> InvokeStatus<int32_t> {
        // invoke method 0 three times, with various gas costs
        auto [r1, g1] =
            handler.invoke(Invocation{.invoked_address = address,
                                      .method = 0,
                                      .gas_limit = UINT64_MAX,
                                      .calldata = {100, 0, 0, 0}},
                           UINT64_MAX,
                           std::to_underlying(InvokeFlag::kInvokeFlagNone));
        if (!r1.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (g1 != 100) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        auto [r2, g2] =
            handler.invoke(Invocation{.invoked_address = address,
                                      .method = 0,
                                      .gas_limit = UINT64_MAX,
                                      .calldata = {200, 0, 0, 0}},
                           UINT64_MAX,
                           std::to_underlying(InvokeFlag::kInvokeFlagNone));
        if (!r2.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (g2 != 200) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        auto [r3, g3] =
            handler.invoke(Invocation{.invoked_address = address,
                                      .method = 0,
                                      .gas_limit = UINT64_MAX,
                                      .calldata = {150, 0, 0, 0}},
                           UINT64_MAX,
                           std::to_underlying(InvokeFlag::kInvokeFlagNone));
        if (!r3.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (g3 != 150) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        if (!runtime.consume_gas(g1 + g2 + g3)) {
          return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
        }

        return 0;
      });

  builder.add(2,
              [&address](SystemCallHandler &handler,
                         NativeRuntime &runtime) -> InvokeStatus<int32_t> {
                auto [r, g] =
                    handler.invoke(Invocation{.invoked_address = address,
                                              .method = 1,
                                              .gas_limit = UINT64_MAX,
                                              .calldata = {}},
                                   UINT64_MAX,
                                   std::to_underlying(
                                       InvokeFlag::kInvokeFlagNone));
                if (!r.has_value()) {
                  return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                }
                if (g != 100 + 200 + 150) {
                  return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                }
                if (!runtime.consume_gas(g)) {
                  return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
                }

                return 0;
              });

  auto native_contract = builder.build();

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));

  runtime_context_ = std::make_shared<NativeContext>(std::move(contract_map));

  StorageSnapshotCache snapshot(0, *db_proxy_);

  Contract contract(ContractType::kNative);
  contract.native_contract_id() = 0;

  snapshot_server_.setup_create_contract(contract);
  snapshot_server_.setup_create_deployment(address, sha256(contract));

  SystemCallHandler handler(
      SignedTransaction{.tx =
                            Transaction{
                                .invocation =
                                    {
                                        .invoked_address = address,
                                        .method = 2,
                                        .gas_limit = UINT64_MAX,
                                        .calldata = {},
                                    },
                            }},
      snapshot, PerTransactionDatabaseProxy(*db_proxy_, 0, 0),
      ProposalModeData{}, runtime_context_);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.result_);
  ASSERT_EQ(*res.result_, 0);
  ASSERT_EQ(res.gas_consumed_, 100 + 200 + 150);
}

} // namespace hogd
