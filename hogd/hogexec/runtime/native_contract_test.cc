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
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class NativeContractTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    auto contract_0 = std::make_unique<NativeContract>(
        std::unordered_map<MethodID, NativeContract::function_t>{
            {0,
             [](SystemCallHandler & /*unused*/) -> InvokeStatus<int32_t> {
               return 0;
             }},
            {1, [](SystemCallHandler & /*unused*/) -> InvokeStatus<int32_t> {
               return InvokeError{InvokeErrorCode::kDeterministic};
             }}});

    // Invokes another contract, returns the error code of the other contract
    auto contract_1 = std::make_unique<NativeContract>(
        std::unordered_map<MethodID, NativeContract::function_t>{
            {0, [](SystemCallHandler &handler) -> InvokeStatus<int32_t> {
               auto const &calldata = handler.get_calldata();

               if (calldata.size() != 36) {
                 return InvokeError{InvokeErrorCode::kDeterministic};
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
               return InvokeError{InvokeErrorCode::kDeterministic};
             }}});

    std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(contract_0));
    contract_map.emplace(1, std::move(contract_1));

    runtime_context_ = std::make_shared<NativeContext>(std::move(contract_map));
  }

  std::shared_ptr<RuntimeContext> runtime_context_;
};

TEST_F(NativeContractTest, BasicInstantiate)
{
  StorageSnapshotCache snapshot;
  SystemCallHandler handler(SignedTransaction{}, snapshot, runtime_context_);

  Contract contract(ContractType::kNative);

  auto runtime = runtime_context_->make_new_runtime(contract, &handler, true);

  auto res = runtime->invoke(0, UINT64_MAX);

  ASSERT_TRUE(res.result_);
  ASSERT_EQ(*res.result_, 0);

  res = runtime->invoke(1, UINT64_MAX);

  ASSERT_FALSE(res.result_);
  ASSERT_EQ(res.result_.error(), InvokeErrorCode::kDeterministic);
}

} // namespace hogd
