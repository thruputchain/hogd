#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// Tests to make sure that cancelling a block in the db is handled gracefully.

// NOLINTNEXTLINE(misc-use-internal-linkage)
class BlockCancelTest : public ::testing::Test,
                        public NativeContractTestFramework {
public:
  BlockCancelTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

  constexpr static ContractAddress kContractAddress = make_contract_address(0);

  auto make_new_per_tx_db_proxy()
  {
    return NativeContractTestFramework::make_new_per_tx_db_proxy(UINT64_MAX);
  }
};

// Test that runs the root_invoke portion of a transaction,
// then fails when the db has advanced beyond the transaction's block.
TEST_F(BlockCancelTest, CancelBlockBeforeCommit)
{
  // Create a simple native contract that does nothing
  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(0,
                [](NativeSyscallContext & /*unused*/) noexcept
                    -> InvokeStatus<void> { return {}; });
    auto native_contract = std::make_unique<NativeContract>(std::move(fns));
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
// Caller try_invokes a writer contract (no self-reentrance).
TEST_F(BlockCancelTest, CancelDuringTryCommit)
{
  ContractAddress addr_writer = make_contract_address(0);
  ContractAddress addr_caller = make_contract_address(1);

  auto make_context = [addr_writer]() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_writer;
    fns_writer.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          auto key = make_storage_key("test");
          auto [res, _gas] = syscall_BytestringSet_inner(
              ctx.handler(), key, std::as_bytes(std::span{"hello"}),
              std::to_underlying(StorageDeltaFlags::None));
          if (!res.has_value() ||
              res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          return {};
        });
    auto contract_writer =
        std::make_unique<NativeContract>(std::move(fns_writer));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_caller;
    fns_caller.emplace(
        0,
        [addr_writer](
            NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          auto r0 = [&]() {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            return ctx.syscall<SyscallNo::kInvoke>(addr_writer,
                                                   std::span<const std::byte>{},
                                                   std::span<uint32_t>{},
                                                   inputs,
                                                   CopyableInvokeOutputs{});
          }();

          if (r0.has_value() || r0.error().code() != HostFnErrorCode::Abandon) {
            ADD_FAILURE() << "Unexpected return value from try_invoke: "
                          << r0.error().error_code().message();
            return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          return InvokeError{InvokeErrorCode::Abandon};
        });
    auto contract_caller =
        std::make_unique<NativeContract>(std::move(fns_caller));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(contract_writer));
    contract_map.emplace(1, std::move(contract_caller));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  ContractAddress addrs[] = {addr_writer, addr_caller};
  for (int i = 0; i < 2; ++i) {
    auto contract = make_native_contract(i);
    write_contract(contract);
    write_deployment(addrs[i], contract);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  // Create a transaction and call root_invoke()
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = addr_caller,
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
  ASSERT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::Abandon});
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
