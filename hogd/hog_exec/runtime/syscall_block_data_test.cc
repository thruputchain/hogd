#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/native_contract_builder.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/serialize_calldata.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
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
          TestDB::kHogDb)
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

// The timepoint comes from the BlockExecutionContext the block was opened
// with, not from the db tip -- the tip is the parent block, whose timepoint
// belongs to the previous block. This framework builds a snapshot cache
// directly, so the test installs the context the way an executor would.
TEST_F(SyscallBlockDataTest, GetBlockTimepointReturnsOpenedContext)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  constexpr TimePoint kExpectedTimepoint = 1'724'000'000;
  get_storage_snapshot_cache()->set_block_execution_context(
      BlockExecutionContext{.block_height = 1,
                            .block_timepoint = kExpectedTimepoint});

  NativeContractBuilder builder;
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0,
      [](NativeSyscallContext &ctx,
         TimePoint expected_timepoint) -> InvokeStatus<void> {
        TimePoint block_timepoint = 0;
        auto status =
            ctx.syscall<SyscallNo::kGetBlockTimepoint>(block_timepoint);

        if (!status.has_value() ||
            status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (block_timepoint != expected_timepoint) {
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
          kExpectedTimepoint);
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
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

// A scheduler that supplies a height disagreeing with the tip is executing
// against a different block than the one it is building. The transaction is
// refused outright rather than run against either value.
//
// The check lives at the pin point (init_db_tip), not in GetBlockHeight, so
// it fires during admission and the contract body below never runs -- that is
// the point: the disagreement is caught before any contract can observe a
// height, rather than reaching consensus.
TEST_F(SyscallBlockDataTest, MismatchedContextHeightRejectsTransaction)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // The tip implies executing height 1 (see GetBlockHeightSuccess above).
  get_storage_snapshot_cache()->set_block_execution_context(
      BlockExecutionContext{.block_height = 99, .block_timepoint = 1});

  NativeContractBuilder builder;
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        // Never reached: admission refuses the transaction first. Kept as a
        // tripwire -- reporting success here would mean the mismatch went
        // unnoticed and a contract ran against a height nobody agreed on.
        BlockHeight block_height = 0;
        auto status = ctx.syscall<SyscallNo::kGetBlockHeight>(block_height);
        if (status.has_value() &&
            status.value() == SyscallErrorCode::kSyscallErrorCodeNone) {
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

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *context);

  // The mismatch surfaces as an unrecoverable system error out of the
  // transaction's initial validation, so the root invocation fails.
  ASSERT_FALSE(handler.root_invoke().is_root_success());
}

// The two syscalls above are read-only and take no inputs, so their whole
// contribution to a transaction trace is the value they returned. These pin
// that the trace records it, and records the value the contract actually saw
// -- a trace that reported a different height or timepoint than the block
// executed against would misexplain every transaction replayed from it.
TEST_F(SyscallBlockDataTest, GetBlockHeightIsTraced)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Built on top of the committed genesis tip (height 0).
  constexpr BlockHeight kExpectedHeight = 1;

  NativeContractBuilder builder;
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        BlockHeight block_height = 0;
        auto status = ctx.syscall<SyscallNo::kGetBlockHeight>(block_height);
        if (!status.has_value() ||
            status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, builder.build());
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *context);
  handler.enable_tracing();

  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);

  auto const *trace = handler.get_trace();
  ASSERT_NE(trace, nullptr);
  ASSERT_EQ(trace->system_calls.size(), 1U);

  auto const &call = trace->system_calls[0];
  ASSERT_EQ(call.body.syscall_number(), SyscallNo::kGetBlockHeight);
  ASSERT_EQ(call.status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeNone);

  auto const &traced = call.body.get_block_height();
  ASSERT_NE(traced.outputs, nullptr);
  EXPECT_EQ(traced.outputs->block_height, kExpectedHeight);
}

TEST_F(SyscallBlockDataTest, GetBlockTimepointIsTraced)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Unlike the height, the timepoint is not derivable from the tip, so the
  // traced value has to be the one the block was opened with. Set a distinct
  // one so a trace echoing some other source (the parent's timepoint, a
  // default) cannot pass.
  constexpr TimePoint kExpectedTimepoint = 1'724'000'000;
  get_storage_snapshot_cache()->set_block_execution_context(
      BlockExecutionContext{.block_height = 1,
                            .block_timepoint = kExpectedTimepoint});

  NativeContractBuilder builder;
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        TimePoint block_timepoint = 0;
        auto status =
            ctx.syscall<SyscallNo::kGetBlockTimepoint>(block_timepoint);
        if (!status.has_value() ||
            status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, builder.build());
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *context);
  handler.enable_tracing();

  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);

  auto const *trace = handler.get_trace();
  ASSERT_NE(trace, nullptr);
  ASSERT_EQ(trace->system_calls.size(), 1U);

  auto const &call = trace->system_calls[0];
  ASSERT_EQ(call.body.syscall_number(), SyscallNo::kGetBlockTimepoint);
  ASSERT_EQ(call.status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeNone);

  auto const &traced = call.body.get_block_timepoint();
  ASSERT_NE(traced.outputs, nullptr);
  EXPECT_EQ(traced.outputs->block_timepoint, kExpectedTimepoint);
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
