#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// Tests to make sure that cancelling a block in the db is handled gracefully.

class BlockCancelTest : public ::testing::Test,
                        public NativeContractTestFramework {
public:
  BlockCancelTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

  constexpr static ContractAddress kContractAddress = make_contract_address(0);
};

// Test that runs the root_invoke portion of a transaction,
// then fails when the db has advanced beyond the transaction's block.
TEST_F(BlockCancelTest, CancelBlockBeforeCommit)
{
  // Create a simple native contract that does nothing
  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    auto native_contract = std::make_unique<NativeContract>(
        std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
            {0,
             [](SystemCallHandler & /*unused*/, NativeRuntime & /*unused*/)
                 -> InvokeStatus<void> { return {}; }}});
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  // Deploy the contract and grant storage tokens
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(kContractAddress, contract);
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  // Create a transaction and call root_invoke()
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kContractAddress,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  // Advance the block - this closes and commits the current block,
  // making the transaction's block no longer current
  advance_block();

  // Try to commit the transaction - this should fail because the block
  // the transaction was created for is no longer current
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultSystemError);
}

// Gracefully handle an error found at try_commit_transaction time.
// One frame invokes another in try_invoke
TEST_F(BlockCancelTest, CancelDuringTryCommit)
{
  // Create a contract where method 0 invokes method 1 using try_invoke
  // Method 1 performs a storage write
  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    auto native_contract = std::make_unique<NativeContract>(
        std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
            {0,
             [](SystemCallHandler &handler,
                NativeRuntime &runtime) -> InvokeStatus<void> {
               // Invoke method 1 using try_invoke
               auto [r0, _g0] = handler.invoke(
                   handler.get_current_address(), 1,
                   runtime.get_available_gas(), handler.get_calldata(),
                   std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));

               if (r0.has_value() ||
                   r0.error().code() !=
                       HostFnErrorCode::kHostAbortTransaction) {
                 ADD_FAILURE() << "Unexpected return value from try_invoke: "
                               << r0.error().error_code().message();
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               return InvokeError{InvokeErrorCode::kInvokeAbortTransaction};
             }},
            {1,
             [](SystemCallHandler &handler,
                NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               // Perform a storage write
               auto key = make_storage_key("test");
               auto [res, _gas] = syscall_BytestringSet_inner(
                   handler, key, std::as_bytes(std::span{"hello"}),
                   std::to_underlying(
                       StorageDeltaFlags::kStorageDeltaFlagNone));
               if (!res.has_value() ||
                   res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }

               return {};
             }}});
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  // Deploy the contract and grant storage tokens
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(kContractAddress, contract);
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  // Create a transaction and call root_invoke()
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kContractAddress,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *context);

  // keep a reference to the storage snapshot cache to avoid invalidating it
  // when the block is advanced
  std::shared_ptr<StorageSnapshotCache> storage_snapshot_cache =
      get_storage_snapshot_cache();
  // Advance the block - this closes and commits the current block,
  // making the transaction's block no longer current
  advance_block();
  auto res = handler.root_invoke();

  ASSERT_FALSE(res.result_.has_value());
  ASSERT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeAbortTransaction});
}

} // namespace hogd
