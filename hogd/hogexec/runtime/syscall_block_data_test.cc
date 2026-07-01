#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class SyscallBlockDataTest : public ::testing::Test,
                             public NativeContractTestFramework {
public:
  SyscallBlockDataTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// Test GetBlockHeight syscall returns the correct block height
TEST_F(SyscallBlockDataTest, GetBlockHeightSuccess)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // The tx executes in the block being built on top of genesis (committed tip
  // 0), i.e. at executing height 1.
  BlockHeight expected_height = 1;

  // Create a native contract with a method that calls GetBlockHeight and
  // verifies it matches the expected height passed as calldata
  NativeContractBuilder builder;

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0,
      [](NativeSyscallContext &ctx,
         BlockHeight expected_height) -> InvokeStatus<void> {
        // Call GetBlockHeight syscall
        BlockHeight block_height = 100;
        auto status = ctx.syscall<SyscallNo::kGetBlockHeight>(block_height);

        if (!status.has_value() ||
            status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Verify the returned height matches expected
        if (block_height != expected_height) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        return {};
      });

  auto native_contract = builder.build();

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  // Serialize expected height as calldata
  auto calldata =
      Serializer<NativeContractBuilder::kDefaultSerializationFormat>::serialize(
          expected_height);
  ASSERT_TRUE(calldata.has_value());

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = calldata.value()},
                            .uniquifier = make_new_uniquifier()}},
      *context);

  // Invoke the contract method which calls the syscall and verifies the height
  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());

  // Commit the transaction
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test GetBlockHeight after advancing blocks multiple times
TEST_F(SyscallBlockDataTest, GetBlockHeightAfterMultipleAdvances)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Advance blocks multiple times
  advance_block();
  advance_block();
  advance_block();

  // After 3 advances from genesis, committed tip is height 3; the tx we run now
  // executes in the block being built on top of it, i.e. at executing height 4.
  BlockHeight expected_height = 4;

  // Create a native contract with a method that calls GetBlockHeight and
  // verifies it matches the expected height passed as calldata
  NativeContractBuilder builder;

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0,
      [](NativeSyscallContext &ctx,
         BlockHeight expected_height) -> InvokeStatus<void> {
        // Call GetBlockHeight syscall
        BlockHeight block_height = 0;
        auto status = ctx.syscall<SyscallNo::kGetBlockHeight>(block_height);

        if (!status.has_value() ||
            status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Verify the returned height matches expected
        if (block_height != expected_height) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        return {};
      });

  auto native_contract = builder.build();

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  // Serialize expected height as calldata
  auto calldata =
      Serializer<NativeContractBuilder::kDefaultSerializationFormat>::serialize(
          expected_height);
  ASSERT_TRUE(calldata.has_value());

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = calldata.value()},
                            .uniquifier = make_new_uniquifier()}},
      *context);

  // Invoke the contract method which calls the syscall and verifies the height
  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());

  // Commit the transaction
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Regression for #2847: GetBlockHeight must report the *executing* block height
// (parent tip + 1), not the last-committed parent tip. A transaction runs in
// the block being built on top of committed height N, i.e. at height N+1, and
// height-gated contract logic (auth-token expiry, timelocks) must observe that.
TEST_F(SyscallBlockDataTest, GetBlockHeightReturnsExecutingHeight)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Commit heights 1 and 2; the transaction we run now executes in the block
  // being built on top of committed tip 2, i.e. at executing height 3.
  advance_block();
  advance_block();

  BlockHeight expected_height = get_block_number();
  ASSERT_EQ(expected_height, BlockHeight{3});

  NativeContractBuilder builder;

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0,
      [](NativeSyscallContext &ctx,
         BlockHeight expected_height) -> InvokeStatus<void> {
        BlockHeight block_height = 0;
        auto status = ctx.syscall<SyscallNo::kGetBlockHeight>(block_height);

        if (!status.has_value() ||
            status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        if (block_height != expected_height) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        return {};
      });

  auto native_contract = builder.build();

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto calldata =
      Serializer<NativeContractBuilder::kDefaultSerializationFormat>::serialize(
          expected_height);
  ASSERT_TRUE(calldata.has_value());

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = calldata.value()},
                            .uniquifier = make_new_uniquifier()}},
      *context);

  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
