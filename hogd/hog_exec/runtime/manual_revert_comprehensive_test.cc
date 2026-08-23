#include <cstddef>
#include <gtest/gtest.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_entry_outer.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

//
// Comprehensive tests for manual frame revert functionality.
//
// Manual revert is a key feature that allows contracts to:
// 1. Revert all state changes made in the current frame
// 2. Return data to the parent caller (unlike normal failures)
// 3. Allow the transaction to commit successfully (unlike normal failures)
//
// The functionality is exposed in the SDK through the Returnable trait:
// - When a contract method returns Result::Err, force_revert() returns true
// - This triggers the Return syscall with revert_frame=true
// - State changes are reverted but error data is serialized and returned
//
// This is similar to EVM's REVERT opcode behavior.
//

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class ManualRevertTest : public ::testing::TestWithParam<TestDB>,
                         public NativeContractTestFramework {
public:
  ManualRevertTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

//
// Test basic manual frame revert functionality.
//
// Verifies that when a contract uses manual revert (Return syscall with
// revert_frame=true):
// - State changes within the frame are reverted
// - The transaction still succeeds (unlike normal failures)
// - Return data is preserved and passed to the caller
// - Gas consumption is properly tracked
//
TEST_P(ManualRevertTest, ManualRevertBasicFunctionality)
{
  ContractAddress contract_addr = make_contract_address(1);
  auto str_hello = make_bytestring("hello");
  auto return_data = make_bytestring("error_occurred");
  auto key = make_storage_key("test_key");

  // Contract that writes state, then reverts with return data
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [str_hello, return_data, key](NativeSyscallContext &ctx) noexcept
                  -> x23::Async<InvokeStatus<void>> {
                // Write state that should be reverted
                auto [res, _gas] = syscall_BytestringSet_inner(
                    ctx.handler(), key, std::as_bytes(std::span(str_hello)),
                    std::to_underlying(StorageDeltaFlags::None))();
                if (!res.has_value() ||
                    res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                  ADD_FAILURE()
                      << "ManualRevertBasicFunctionality: Failed to set "
                         "bytestring in method 0";
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }

                // Manually revert with co_return data
                ReturnBuffer buffer;
                buffer.resize_uninitialized(return_data.size());
                std::memcpy(buffer.data(), return_data.data(),
                            return_data.size());
                ctx.handler().set_return_buffer(std::move(buffer), true);
                co_return InvokeStatus<void>{};
              });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Deploy contract
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Invoke the contract
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  // Error code is the main external difference between root frame revert and
  // normal success
  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultTxRootFrameReverted);

  advance_block();

  // State should be reverted (bytestring not written)
  auto data_addrkey = make_contract_data_addrkey(contract_addr, "test_key");
  auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
  ASSERT_FALSE(data_item_opt.has_value());
}

//
// Test nested manual revert with return data preservation.
//
// Two deployed contracts at separate addresses. Parent invokes child (regular
// invoke, not try_invoke). Child writes state then performs manual revert with
// custom return data. Parent observes the invoke succeed with an Abort
// frame_status, reads the return buffer,
// and finishes successfully. Verifies parent's storage persists and child's
// storage key is absent after commit.
//
TEST_P(ManualRevertTest, NestedManualRevertWithReturnData)
{
  constexpr ContractAddress kAddressParent = make_contract_address(1);
  constexpr ContractAddress kAddressChild = make_contract_address(2);

  auto parent_data = make_bytestring("parent_state");
  auto child_data = make_bytestring("child_state");
  auto child_return_data = make_bytestring("child_error");
  auto parent_key = make_storage_key("parent_key");
  auto child_key = make_storage_key("child_key");

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_parent;
  fns_parent.emplace(
      0,
      [parent_data, parent_key,
       kAddressChild](NativeSyscallContext &ctx) noexcept
          -> x23::Async<InvokeStatus<void>> {
        auto [res, _gas] = syscall_BytestringSet_inner(
            ctx.handler(), parent_key, std::as_bytes(std::span(parent_data)),
            std::to_underlying(StorageDeltaFlags::None))();
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "NestedManualRevertWithReturnData: Failed to "
                           "set bytestring in parent contract";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        CopyableInvokeOutputs r1_outputs{};
        auto r1 = co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = 0,
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              kAddressChild, std::span<const std::byte>{},
              std::span<uint32_t>{}, inputs, r1_outputs);
        }();
        if (!r1.has_value()) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (r1_outputs.frame_status != InvocationFrameStatusCode::Abort) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto status = ctx.handler().get_return_buffer();
        if (!status || !(*status)) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto expected_child_data = make_bytestring("child_error");
        if ((**status)->size() != expected_child_data.size()) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (std::memcmp((**status)->data(), expected_child_data.data(),
                        expected_child_data.size()) != 0) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  auto native_contract_parent =
      std::make_unique<NativeContract>(std::move(fns_parent));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0,
      [child_data, child_return_data,
       child_key](NativeSyscallContext &ctx) noexcept
          -> x23::Async<InvokeStatus<void>> {
        auto [res, _gas] = syscall_BytestringSet_inner(
            ctx.handler(), child_key, std::as_bytes(std::span(child_data)),
            std::to_underlying(StorageDeltaFlags::None))();
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "NestedManualRevertWithReturnData: Failed to "
                           "set bytestring in child contract";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto ret = co_await ctx.syscall<SyscallNo::kReturn>(
            std::as_bytes(std::span(child_return_data)), true,
            std::span<const std::byte>{});

        if (ret.has_value() || ret.error().code() != HostFnErrorCode::Return) {
          ADD_FAILURE() << "NestedManualRevertWithReturnData: Failed to "
                           "co_return from child contract";
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        }
        co_return InvokeStatus<void>{};
      });
  auto native_contract_child =
      std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_parent));
  contract_map.emplace(1, std::move(native_contract_child));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_parent = make_native_contract(0);
  auto contract_child = make_native_contract(1);
  write_contract(contract_parent);
  write_contract(contract_child);
  write_deployment(kAddressParent, contract_parent);
  write_deployment(kAddressChild, contract_child);
  setup_grant_storage_token(kAddressParent, 1'000'000'000);
  setup_grant_storage_token(kAddressChild, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kAddressParent,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  advance_block();

  auto parent_addrkey =
      make_contract_data_addrkey(kAddressParent, "parent_key");
  auto parent_item_opt = lookup_storage_item_from_db(parent_addrkey);
  ASSERT_TRUE(parent_item_opt.has_value());

  auto child_addrkey = make_contract_data_addrkey(kAddressChild, "child_key");
  auto child_item_opt = lookup_storage_item_from_db(child_addrkey);
  ASSERT_FALSE(child_item_opt.has_value());
}

//
// Test the difference between manual revert and normal contract failure.
//
// Compares two scenarios to ensure manual revert behaves differently:
// 1. Manual revert: State reverted, transaction succeeds, return data preserved
// 2. Normal failure: State reverted, transaction fails, no return data
// This test validates that manual revert provides the unique "controlled
// failure" behavior.
//
TEST_P(ManualRevertTest, ManualRevertVsNormalFailure)
{
  ContractAddress contract_addr = make_contract_address(1);
  auto test_data = make_bytestring("test_state");
  auto return_data = make_bytestring("manual_revert_data");
  auto key = make_storage_key("test_key");

  auto make_context = [&]() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    // Method 0: Manual revert with return data
    fns.emplace(0,
                [test_data, return_data,
                 key](NativeSyscallContext &ctx) noexcept
                    -> x23::Async<InvokeStatus<void>> {
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), key, std::as_bytes(std::span(test_data)),
                      std::to_underlying(StorageDeltaFlags::None))();
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE()
                        << "ManualRevertVsNormalFailure: Failed to set "
                           "bytestring in method 0 (manual revert)";
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }

                  ReturnBuffer buffer;
                  buffer.resize_uninitialized(return_data.size());
                  std::memcpy(buffer.data(), return_data.data(),
                              return_data.size());
                  ctx.handler().set_return_buffer(std::move(buffer), true);
                  co_return InvokeStatus<void>{};
                });
    // Method 1: Normal failure
    fns.emplace(1,
                [test_data, key](NativeSyscallContext &ctx) noexcept
                    -> x23::Async<InvokeStatus<void>> {
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), key, std::as_bytes(std::span(test_data)),
                      std::to_underlying(StorageDeltaFlags::None))();
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE()
                        << "ManualRevertVsNormalFailure: Failed to set "
                           "bytestring in method 1 (normal failure)";
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }

                  // Normal failure - no co_return data
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                });
    auto native_contract = std::make_unique<NativeContract>(std::move(fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  // Deploy contract
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Test manual revert (method 0) - should succeed
  {
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

    auto res = handler.root_invoke()();
    ASSERT_TRUE(res.is_root_success()); // Manual revert should succeed

    auto commit_res = handler.try_commit_transaction()();
    // should commit successfully
    ASSERT_EQ(commit_res, ExecResult::ExecResultTxRootFrameReverted);

    advance_block();
  }

  // Test normal failure (method 1) - should fail
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address = contract_addr,
                                             .entrypoint =
                                                 entrypoint_from_method_id(1),
                                             .gas_limit = UINT64_MAX,
                                             .calldata = {}},
                              .uniquifier = make_new_uniquifier()}},
        *context);

    auto res = handler.root_invoke()();
    ASSERT_FALSE(res.is_root_success()); // Normal failure should fail

    auto commit_res = handler.try_commit_transaction()();
    // should not commit
    ASSERT_EQ(commit_res, ExecResult::ExecResultTxDeterministicError);

    advance_block();
  }

  // In both cases, state should not persist
  auto data_addrkey = make_contract_data_addrkey(contract_addr, "test_key");
  auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
  ASSERT_FALSE(data_item_opt.has_value());
}

//
// Test gas consumption behavior with manual revert.
//
// Verifies that:
// - Gas consumed before manual revert is properly accounted for
// - Manual revert operation itself consumes appropriate gas
// - Gas consumption is consistent between manual revert and normal execution
// - Out-of-gas scenarios are handled correctly with manual revert
//
TEST_P(ManualRevertTest, GasConsumptionWithManualRevert)
{
  ContractAddress contract_addr = make_contract_address(1);
  auto test_data = make_bytestring("gas_test");
  auto return_data = make_bytestring("revert_data");

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [test_data, return_data](NativeSyscallContext &ctx) noexcept
                  -> x23::Async<InvokeStatus<void>> {
                // Do expensive work before reverting
                for (int i = 0; i < 10; ++i) {
                  auto temp_key = make_storage_key("temp_" + std::to_string(i));
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), temp_key,
                      std::as_bytes(std::span(test_data)),
                      std::to_underlying(StorageDeltaFlags::None))();
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE()
                        << "GasConsumptionWithManualRevert: Failed to "
                           "set bytestring in method 0, iteration "
                        << i;
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }

                  if (!ctx.consume_gas(1000)) {
                    ADD_FAILURE() << "Failed to consume gas";
                  }
                }

                // Manual revert with co_return data
                ReturnBuffer buffer;
                buffer.resize_uninitialized(return_data.size());
                std::memcpy(buffer.data(), return_data.data(),
                            return_data.size());
                ctx.handler().set_return_buffer(std::move(buffer), true);
                co_return InvokeStatus<void>{};
              });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Deploy contract
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Invoke with limited gas
  const uint64_t gas_limit = 1'000'000;
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = gas_limit,
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  // Gas consumption behavior (native contracts may or may not consume gas in
  // tests)
  auto gas_consumed = res.gas_consumed_;
  ASSERT_LT(gas_consumed, gas_limit); // Should not exceed limit
  ASSERT_GE(gas_consumed, 10000);

  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultTxRootFrameReverted);

  advance_block();

  // All state should be reverted
  for (int i = 0; i < 10; ++i) {
    auto temp_addrkey =
        make_contract_data_addrkey(contract_addr, "temp_" + std::to_string(i));
    auto temp_item_opt = lookup_storage_item_from_db(temp_addrkey);
    ASSERT_FALSE(temp_item_opt.has_value());
  }
}

//
// Test multiple levels of nested manual reverts.
//
// Deploys three native contracts at three addresses (A, B, C). Call chain:
// Contract A:0 → Contract B:0 → Contract C:0. Contract C performs a manual
// revert after writing; B and A each observe a successful invoke with an Abort
// frame_status from the callee,
// then manually revert with their own return buffers. Verifies that storage
// keys level1 / level2 / level3 on the respective contracts are all absent
// after the root frame reverts the transaction.
//
TEST_P(ManualRevertTest, MultipleNestedManualReverts)
{
  constexpr static ContractAddress kAddressA = make_contract_address(1);
  constexpr static ContractAddress kAddressB = make_contract_address(2);
  constexpr static ContractAddress kAddressC = make_contract_address(3);

  auto data1 = make_bytestring("level1");
  auto data2 = make_bytestring("level2");
  auto data3 = make_bytestring("level3");
  auto return1 = make_bytestring("return1");
  auto return2 = make_bytestring("return2");
  auto return3 = make_bytestring("return3");

  // Contract A -- calls B:0, expects an Abort frame_status, then manual revert.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_a;
  fns_a.emplace(0,
                [data1, return1](NativeSyscallContext &ctx) noexcept
                    -> x23::Async<InvokeStatus<void>> {
                  auto key = make_storage_key("level1");
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), key, std::as_bytes(std::span(data1)),
                      std::to_underlying(StorageDeltaFlags::None))();
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE()
                        << "MultipleNestedManualReverts: Failed to set "
                           "bytestring on contract A";
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }

                  CopyableInvokeOutputs r2_outputs{};
                  auto r2 = co_await
                      [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
                    CopyableInvokeInputs inputs{
                        .entrypoint_id = 0,
                        .gas_limit = UINT64_MAX,
                        .flags = 0,
                    };
                    co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                        kAddressB, std::span<const std::byte>{},
                        std::span<uint32_t>{}, inputs, r2_outputs);
                  }();
                  if (!r2.has_value()) {
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  if (r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  if (r2_outputs.frame_status !=
                      InvocationFrameStatusCode::Abort) {
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }

                  ReturnBuffer buffer;
                  buffer.resize_uninitialized(return1.size());
                  std::memcpy(buffer.data(), return1.data(), return1.size());
                  ctx.handler().set_return_buffer(std::move(buffer), true);
                  co_return InvokeStatus<void>{};
                });
  auto native_contract_a = std::make_unique<NativeContract>(std::move(fns_a));

  // Contract B -- calls C:0, expects an Abort frame_status, then manual revert.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_b;
  fns_b.emplace(0,
                [data2, return2](NativeSyscallContext &ctx) noexcept
                    -> x23::Async<InvokeStatus<void>> {
                  auto key = make_storage_key("level2");
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), key, std::as_bytes(std::span(data2)),
                      std::to_underlying(StorageDeltaFlags::None))();
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE()
                        << "MultipleNestedManualReverts: Failed to set "
                           "bytestring on contract B";
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }

                  CopyableInvokeOutputs r3_outputs{};
                  auto r3 = co_await
                      [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
                    CopyableInvokeInputs inputs{
                        .entrypoint_id = 0,
                        .gas_limit = UINT64_MAX,
                        .flags = 0,
                    };
                    co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                        kAddressC, std::span<const std::byte>{},
                        std::span<uint32_t>{}, inputs, r3_outputs);
                  }();
                  if (!r3.has_value()) {
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  if (r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  if (r3_outputs.frame_status !=
                      InvocationFrameStatusCode::Abort) {
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }

                  ReturnBuffer buffer;
                  buffer.resize_uninitialized(return2.size());
                  std::memcpy(buffer.data(), return2.data(), return2.size());
                  ctx.handler().set_return_buffer(std::move(buffer), true);
                  co_return InvokeStatus<void>{};
                });
  auto native_contract_b = std::make_unique<NativeContract>(std::move(fns_b));

  // Contract C -- deepest level; write then manual revert.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_c;
  fns_c.emplace(0,
                [data3, return3](NativeSyscallContext &ctx) noexcept
                    -> x23::Async<InvokeStatus<void>> {
                  auto key = make_storage_key("level3");
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), key, std::as_bytes(std::span(data3)),
                      std::to_underlying(StorageDeltaFlags::None))();
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE()
                        << "MultipleNestedManualReverts: Failed to set "
                           "bytestring on contract C";
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }

                  ReturnBuffer buffer;
                  buffer.resize_uninitialized(return3.size());
                  std::memcpy(buffer.data(), return3.data(), return3.size());
                  ctx.handler().set_return_buffer(std::move(buffer), true);
                  co_return InvokeStatus<void>{};
                });
  auto native_contract_c = std::make_unique<NativeContract>(std::move(fns_c));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_a));
  contract_map.emplace(1, std::move(native_contract_b));
  contract_map.emplace(2, std::move(native_contract_c));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_a = make_native_contract(0);
  auto contract_b = make_native_contract(1);
  auto contract_c = make_native_contract(2);
  write_contract(contract_a);
  write_contract(contract_b);
  write_contract(contract_c);
  write_deployment(kAddressA, contract_a);
  write_deployment(kAddressB, contract_b);
  write_deployment(kAddressC, contract_c);
  setup_grant_storage_token(kAddressA, 1'000'000'000);
  setup_grant_storage_token(kAddressB, 1'000'000'000);
  setup_grant_storage_token(kAddressC, 1'000'000'000);

  // Root: invoke contract A method 0
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kAddressA,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultTxRootFrameReverted);

  advance_block();

  // All levels should be reverted (each key lives on its contract's address)
  auto key1 = make_contract_data_addrkey(kAddressA, "level1");
  auto key2 = make_contract_data_addrkey(kAddressB, "level2");
  auto key3 = make_contract_data_addrkey(kAddressC, "level3");

  ASSERT_FALSE(lookup_storage_item_from_db(key1).has_value());
  ASSERT_FALSE(lookup_storage_item_from_db(key2).has_value());
  ASSERT_FALSE(lookup_storage_item_from_db(key3).has_value());
}

//
// Test different error types all trigger manual revert correctly.
//
// Verifies that various error conditions (syscall errors, contract errors,
// caller errors) all properly trigger manual revert through the Returnable
// trait mechanism. Tests multiple error scenarios:
// - Syscall errors (kStorageKeyNotExist, kInsufficientGasForInvocation)
// - Custom contract errors (internal_err)
// - Caller errors (caller_error)
// - Normal success case (error_type = 0)
//
TEST_P(ManualRevertTest, ErrorTypeManualReverts)
{
  ContractAddress contract_addr = make_contract_address(1);
  auto test_data = make_bytestring("error_test_data");
  auto key = make_storage_key("error_type_test");

  auto make_context = [&]() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(0,
                [test_data, key](NativeSyscallContext &ctx) noexcept
                    -> x23::Async<InvokeStatus<void>> {
                  // Write state that should be reverted on error
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), key, std::as_bytes(std::span(test_data)),
                      std::to_underlying(StorageDeltaFlags::None))();
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE() << "ErrorTypeManualReverts: Failed to set "
                                     "bytestring in method 0";
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }

                  // Simulate syscall error - this should trigger manual revert
                  ReturnBuffer buffer;
                  auto error_msg = make_bytestring("Storage key not found");
                  buffer.resize_uninitialized(error_msg.size());
                  std::memcpy(buffer.data(), error_msg.data(),
                              error_msg.size());
                  ctx.handler().set_return_buffer(std::move(buffer), true);
                  co_return InvokeStatus<void>{};
                });
    fns.emplace(1,
                [test_data, key](NativeSyscallContext &ctx) noexcept
                    -> x23::Async<InvokeStatus<void>> {
                  // Write state that should be reverted on error
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), key, std::as_bytes(std::span(test_data)),
                      std::to_underlying(StorageDeltaFlags::None))();
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE() << "ErrorTypeManualReverts: Failed to set "
                                     "bytestring in method 1";
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }

                  // Simulate insufficient gas error - this should trigger
                  // manual revert
                  ReturnBuffer buffer;
                  auto error_msg =
                      make_bytestring("Insufficient gas for invocation");
                  buffer.resize_uninitialized(error_msg.size());
                  std::memcpy(buffer.data(), error_msg.data(),
                              error_msg.size());
                  ctx.handler().set_return_buffer(std::move(buffer), true);
                  co_return InvokeStatus<void>{};
                });
    fns.emplace(2,
                [test_data, key](NativeSyscallContext &ctx) noexcept
                    -> x23::Async<InvokeStatus<void>> {
                  // Write state that should be reverted on error
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), key, std::as_bytes(std::span(test_data)),
                      std::to_underlying(StorageDeltaFlags::None))();
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE() << "ErrorTypeManualReverts: Failed to set "
                                     "bytestring in method 2";
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }

                  // Simulate custom contract error - this should trigger manual
                  // revert
                  ReturnBuffer buffer;
                  auto error_msg =
                      make_bytestring("Test custom contract error");
                  buffer.resize_uninitialized(error_msg.size());
                  std::memcpy(buffer.data(), error_msg.data(),
                              error_msg.size());
                  ctx.handler().set_return_buffer(std::move(buffer), true);
                  co_return InvokeStatus<void>{};
                });
    fns.emplace(3,
                [test_data, key](NativeSyscallContext &ctx) noexcept
                    -> x23::Async<InvokeStatus<void>> {
                  // Write state that should NOT be reverted (no error)
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), key, std::as_bytes(std::span(test_data)),
                      std::to_underlying(StorageDeltaFlags::None))();
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE() << "ErrorTypeManualReverts: Failed to set "
                                     "bytestring in method 3";
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }

                  // No error - state should persist
                  co_return InvokeStatus<void>{};
                });
    auto native_contract = std::make_unique<NativeContract>(std::move(fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  // Deploy contract
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto context = make_context();

  // Test error types 0-2 (should all trigger manual revert and succeed)
  for (uint32_t error_type = 0; error_type < 3; ++error_type) {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address = contract_addr,
                                             .entrypoint =
                                                 entrypoint_from_method_id(
                                                     error_type),
                                             .gas_limit = UINT64_MAX,
                                             .calldata = {}},
                              .uniquifier = make_new_uniquifier()}},
        *context);

    auto res = handler.root_invoke()();
    ASSERT_TRUE(res.is_root_success())
        << "Error type " << error_type << " should succeed via manual revert";

    auto commit_res = handler.try_commit_transaction()();
    ASSERT_EQ(commit_res, ExecResult::ExecResultTxRootFrameReverted)
        << "Error type " << error_type << " transaction should commit";

    advance_block();

    // State should be reverted for error types 0-2
    auto data_addrkey =
        make_contract_data_addrkey(contract_addr, "error_type_test");
    auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
    ASSERT_FALSE(data_item_opt.has_value())
        << "Error type " << error_type << " state should be reverted";
  }

  // Test error type 3 (no error - should succeed and keep state)
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address = contract_addr,
                                             .entrypoint =
                                                 entrypoint_from_method_id(3),
                                             .gas_limit = UINT64_MAX,
                                             .calldata = {}},
                              .uniquifier = make_new_uniquifier()}},
        *context);

    auto res = handler.root_invoke()();
    ASSERT_TRUE(res.is_root_success())
        << "No error case should succeed normally";

    auto commit_res = handler.try_commit_transaction()();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK)
        << "No error case transaction should commit";

    advance_block();

    // State should persist for no error case
    auto data_addrkey =
        make_contract_data_addrkey(contract_addr, "error_type_test");
    auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
    ASSERT_TRUE(data_item_opt.has_value())
        << "No error case state should persist";
  }
}

//
// Test manual revert with successful try_invoke children.
//
// Three deployed contracts (parent, child, grandchild) at separate addresses.
// Parent:0 writes parent_key, try_invokes child:0, checks return buffer.
// Child:0 writes child_key, try_invokes grandchild:0 (succeeds), then manual
// revert with return data. Grandchild:0 writes grandchild_key and returns
// normally.
//
// Verify after commit:
// - Parent's state persists (parent_key on parent's address)
// - Child's state is reverted (child_key absent on child's address)
// - Grandchild's state is reverted (grandchild_key absent on grandchild's
//   address — nested under reverted child frame)
// - Return data from child is available to parent
//
TEST_P(ManualRevertTest, ManualRevertWithSuccessfulTryInvokeChildren)
{
  constexpr static ContractAddress kAddressParent = make_contract_address(1);
  constexpr static ContractAddress kAddressChild = make_contract_address(2);
  constexpr static ContractAddress kAddressGrandchild =
      make_contract_address(3);

  auto parent_data = make_bytestring("parent_state");
  auto child_data = make_bytestring("child_state");
  auto grandchild_data = make_bytestring("grandchild_state");
  auto child_return_data = make_bytestring("child_reverted");
  auto parent_key = make_storage_key("parent_key");
  auto child_key = make_storage_key("child_key");
  auto grandchild_key = make_storage_key("grandchild_key");

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_parent;
  fns_parent.emplace(
      0,
      [parent_data, parent_key](NativeSyscallContext &ctx) noexcept
          -> x23::Async<InvokeStatus<void>> {
        auto [res, _gas] = syscall_BytestringSet_inner(
            ctx.handler(), parent_key, std::as_bytes(std::span(parent_data)),
            std::to_underlying(StorageDeltaFlags::None))();
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Failed to set bytestring in parent contract";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        CopyableInvokeOutputs r1_outputs{};
        auto r1 = co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              kAddressChild, std::span<const std::byte>{},
              std::span<uint32_t>{}, inputs, r1_outputs);
        }();
        if (!r1.has_value()) {
          ADD_FAILURE() << "try_invoke on child failed unexpectedly";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Expected kSyscallErrorCodeNone from child";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (r1_outputs.frame_status != InvocationFrameStatusCode::Abort) {
          ADD_FAILURE() << "Expected Abort frame_status from child";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto status = ctx.handler().get_return_buffer();
        if (!status || !(*status)) {
          ADD_FAILURE() << "Child co_return data not available to parent";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto expected = make_bytestring("child_reverted");
        if ((**status)->size() != expected.size() ||
            std::memcmp((**status)->data(), expected.data(), expected.size()) !=
                0) {
          ADD_FAILURE() << "Child co_return data mismatch";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  auto native_contract_parent =
      std::make_unique<NativeContract>(std::move(fns_parent));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0,
      [child_data, child_return_data,
       child_key](NativeSyscallContext &ctx) noexcept
          -> x23::Async<InvokeStatus<void>> {
        auto [res, _gas] = syscall_BytestringSet_inner(
            ctx.handler(), child_key, std::as_bytes(std::span(child_data)),
            std::to_underlying(StorageDeltaFlags::None))();
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Failed to set bytestring in child contract";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto r2 = co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              kAddressGrandchild, std::span<const std::byte>{},
              std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
        }();
        if (!r2.has_value()) {
          ADD_FAILURE() << "try_invoke on grandchild failed unexpectedly";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Expected kSyscallErrorCodeNone from grandchild";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        ReturnBuffer buffer;
        buffer.resize_uninitialized(child_return_data.size());
        std::memcpy(buffer.data(), child_return_data.data(),
                    child_return_data.size());
        ctx.handler().set_return_buffer(std::move(buffer), true);
        co_return InvokeStatus<void>{};
      });
  auto native_contract_child =
      std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_grandchild;
  fns_grandchild.emplace(
      0,
      [grandchild_data, grandchild_key](NativeSyscallContext &ctx) noexcept
          -> x23::Async<InvokeStatus<void>> {
        auto [res, _gas] = syscall_BytestringSet_inner(
            ctx.handler(), grandchild_key,
            std::as_bytes(std::span(grandchild_data)),
            std::to_underlying(StorageDeltaFlags::None))();
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Failed to set bytestring in grandchild contract";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  auto native_contract_grandchild =
      std::make_unique<NativeContract>(std::move(fns_grandchild));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_parent));
  contract_map.emplace(1, std::move(native_contract_child));
  contract_map.emplace(2, std::move(native_contract_grandchild));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_parent = make_native_contract(0);
  auto contract_child = make_native_contract(1);
  auto contract_grandchild = make_native_contract(2);
  write_contract(contract_parent);
  write_contract(contract_child);
  write_contract(contract_grandchild);
  write_deployment(kAddressParent, contract_parent);
  write_deployment(kAddressChild, contract_child);
  write_deployment(kAddressGrandchild, contract_grandchild);
  setup_grant_storage_token(kAddressParent, 1'000'000'000);
  setup_grant_storage_token(kAddressChild, 1'000'000'000);
  setup_grant_storage_token(kAddressGrandchild, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kAddressParent,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  advance_block();

  auto parent_addrkey =
      make_contract_data_addrkey(kAddressParent, "parent_key");
  auto parent_item_opt = lookup_storage_item_from_db(parent_addrkey);
  ASSERT_TRUE(parent_item_opt.has_value())
      << "Parent state should persist after commit";

  auto child_addrkey = make_contract_data_addrkey(kAddressChild, "child_key");
  auto child_item_opt = lookup_storage_item_from_db(child_addrkey);
  ASSERT_FALSE(child_item_opt.has_value())
      << "Child state should be reverted (manual revert discards "
         "modified_items)";

  auto grandchild_addrkey =
      make_contract_data_addrkey(kAddressGrandchild, "grandchild_key");
  auto grandchild_item_opt = lookup_storage_item_from_db(grandchild_addrkey);
  ASSERT_FALSE(grandchild_item_opt.has_value())
      << "Grandchild state should be reverted (nested under reverted child "
         "frame)";
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, ManualRevertTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
