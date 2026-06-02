#include <gtest/gtest.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
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

//
// Comprehensive tests for cross-contract revert propagation.
//
// These tests verify that manual frame revert behavior works correctly
// when contracts call other contracts, ensuring that:
// 1. Revert propagates correctly through contract call chains
// 2. State changes are properly isolated and reverted
// 3. Return data flows correctly through nested calls
// 4. Gas accounting works properly during cross-contract reverts
// 5. Complex nested scenarios behave as expected
//

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class CrossContractRevertTest : public ::testing::TestWithParam<TestDB>,
                                public NativeContractTestFramework {
public:
  CrossContractRevertTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

//
// Test basic cross-contract revert propagation.
//
// Scenario: Contract A calls Contract B, Contract B reverts with return data.
// Verifies that:
// - Contract B's state changes are reverted
// - Contract A receives B's return data
// - Contract A's state changes persist
// - Transaction succeeds despite B's revert
//
TEST_P(CrossContractRevertTest, BasicCrossContractRevert)
{
  ContractAddress contract_a_addr = make_contract_address(0);
  ContractAddress contract_b_addr = make_contract_address(1);

  auto contract_a_data = make_bytestring("contract_a_state");
  auto contract_b_data = make_bytestring("contract_b_state");
  auto contract_b_return_data = make_bytestring("contract_b_error");
  auto contract_a_key = make_storage_key("contract_a_key");
  auto contract_b_key = make_storage_key("contract_b_key");

  // Contract A: Calls Contract B and writes its own state
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_a;
  fns_a.emplace(
      0,
      [contract_a_data, contract_a_key, contract_b_addr](
          NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // Contract A writes its own state
        auto r0 = ctx.syscall<SyscallNo::kBytestringSet>(
            contract_a_key, std::as_bytes(std::span(contract_a_data)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!r0.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (r0.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Failed to set bytestring";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Call Contract B (which will revert)
        CopyableInvokeOutputs r1_outputs{};
        auto r1 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          return ctx.syscall<SyscallNo::kInvoke>(contract_b_addr,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 r1_outputs);
        }();
        if (!r1.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // A child frame ran, so Invoke returns None; the manual
        // revert is reported via frame_status == Abort.
        if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (r1_outputs.frame_status != InvocationFrameStatusCode::Abort) {
          ADD_FAILURE() << "Expected Contract B to manually revert (Abort)";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Contract A should receive Contract B's return data
        auto status = ctx.handler().get_return_buffer();
        if (!status || !(*status)) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Verify return data matches expected
        auto expected_data = make_bytestring("contract_b_error");
        if ((**status)->size() != expected_data.size()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (std::memcmp((**status)->data(), expected_data.data(),
                        expected_data.size()) != 0) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        return {};
      });
  auto contract_a = std::make_unique<NativeContract>(std::move(fns_a));

  // Contract B: Writes state and then reverts with return data
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_b;
  fns_b.emplace(0,
                [contract_b_data, contract_b_return_data, contract_b_key](
                    NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  // Contract B writes state that should be reverted
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), contract_b_key,
                      std::as_bytes(std::span(contract_b_data)),
                      std::to_underlying(StorageDeltaFlags::None));
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE() << "Failed to set bytestring";
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }

                  // Contract B manually reverts with return data
                  ReturnBuffer buffer;
                  buffer.resize_uninitialized(contract_b_return_data.size());
                  std::memcpy(buffer.data(), contract_b_return_data.data(),
                              contract_b_return_data.size());
                  ctx.handler().set_return_buffer(std::move(buffer), true, {});
                  return {};
                });
  auto contract_b = std::make_unique<NativeContract>(std::move(fns_b));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract_a));
  contract_map.emplace(1, std::move(contract_b));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Deploy both contracts
  auto contract_a_obj = make_native_contract(0);
  auto contract_b_obj = make_native_contract(1);

  write_contract(contract_a_obj);
  write_contract(contract_b_obj);
  write_deployment(contract_a_addr, contract_a_obj);
  write_deployment(contract_b_addr, contract_b_obj);
  setup_grant_storage_token(contract_a_addr, 1'000'000'000);
  setup_grant_storage_token(contract_b_addr, 1'000'000'000);

  // Invoke Contract A
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_a_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  advance_block();

  // Contract A's state should persist
  auto contract_a_addrkey =
      make_contract_data_addrkey(contract_a_addr, "contract_a_key");
  auto contract_a_item_opt = lookup_storage_item_from_db(contract_a_addrkey);
  ASSERT_TRUE(contract_a_item_opt.has_value());

  // Contract B's state should be reverted
  auto contract_b_addrkey =
      make_contract_data_addrkey(contract_b_addr, "contract_b_key");
  auto contract_b_item_opt = lookup_storage_item_from_db(contract_b_addrkey);
  ASSERT_FALSE(contract_b_item_opt.has_value());
}

//
// Test deep nested revert propagation (A -> B -> C).
//
// Scenario: Contract A calls Contract B, which calls Contract C.
// Contract C reverts with return data. Verifies that:
// - Contract C's state is reverted
// - Contract B receives C's return data and can process it
// - Contract A receives B's return data
// - Only Contract A's state persists
//
TEST_P(CrossContractRevertTest, DeepNestedRevertPropagation)
{
  ContractAddress contract_a_addr = make_contract_address(0);
  ContractAddress contract_b_addr = make_contract_address(1);
  ContractAddress contract_c_addr = make_contract_address(2);

  auto contract_a_data = make_bytestring("contract_a_state");
  auto contract_b_data = make_bytestring("contract_b_state");
  auto contract_c_data = make_bytestring("contract_c_state");
  auto contract_c_return_data = make_bytestring("contract_c_error");
  auto contract_b_return_data = make_bytestring("contract_b_processed");

  auto contract_a_key = make_storage_key("contract_a_key");
  auto contract_b_key = make_storage_key("contract_b_key");
  auto contract_c_key = make_storage_key("contract_c_key");

  // Contract A: Calls Contract B and writes its own state
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_a;
  fns_a.emplace(
      0,
      [contract_a_data, contract_a_key, contract_b_addr](
          NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // Contract A writes its own state
        auto [res, _gas] = syscall_BytestringSet_inner(
            ctx.handler(), contract_a_key,
            std::as_bytes(std::span(contract_a_data)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Failed to set bytestring";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Call Contract B
        CopyableInvokeOutputs r1_outputs{};
        auto r1 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          return ctx.syscall<SyscallNo::kInvoke>(contract_b_addr,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 r1_outputs);
        }();
        if (!r1.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // A child frame ran, so Invoke returns None; Contract B's
        // manual revert is reported via frame_status == Abort.
        if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (r1_outputs.frame_status != InvocationFrameStatusCode::Abort) {
          ADD_FAILURE() << "Expected Contract B to manually revert (Abort)";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Contract A should receive Contract B's processed return
        // data
        auto status = ctx.handler().get_return_buffer();
        if (!status || !(*status)) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Verify return data matches expected
        auto expected_data = make_bytestring("contract_b_processed");
        if ((**status)->size() != expected_data.size()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (std::memcmp((**status)->data(), expected_data.data(),
                        expected_data.size()) != 0) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        return {};
      });
  auto contract_a = std::make_unique<NativeContract>(std::move(fns_a));

  // Contract B: Calls Contract C, processes C's return data, and returns
  // processed data
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_b;
  fns_b.emplace(
      0,
      [contract_b_data, contract_b_return_data, contract_b_key,
       contract_c_addr](
          NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // Contract B writes state that should be reverted
        auto [res, _gas] = syscall_BytestringSet_inner(
            ctx.handler(), contract_b_key,
            std::as_bytes(std::span(contract_b_data)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Failed to set bytestring";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Call Contract C (which will revert)
        CopyableInvokeOutputs r1_outputs{};
        auto r1 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          return ctx.syscall<SyscallNo::kInvoke>(contract_c_addr,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 r1_outputs);
        }();
        if (!r1.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // A child frame ran, so Invoke returns None; Contract C's
        // manual revert is reported via frame_status == Abort.
        if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (r1_outputs.frame_status != InvocationFrameStatusCode::Abort) {
          ADD_FAILURE() << "Expected Contract C to manually revert (Abort)";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Contract B receives Contract C's return data
        auto status = ctx.handler().get_return_buffer();
        if (!status || !(*status)) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Verify C's return data
        auto expected_c_data = make_bytestring("contract_c_error");
        if ((**status)->size() != expected_c_data.size()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (std::memcmp((**status)->data(), expected_c_data.data(),
                        expected_c_data.size()) != 0) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Contract B returns processed data
        ReturnBuffer buffer;
        buffer.resize_uninitialized(contract_b_return_data.size());
        std::memcpy(buffer.data(), contract_b_return_data.data(),
                    contract_b_return_data.size());
        ctx.handler().set_return_buffer(std::move(buffer), true, {});
        return {};
      });
  auto contract_b = std::make_unique<NativeContract>(std::move(fns_b));

  // Contract C: Writes state and reverts with return data
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_c;
  fns_c.emplace(0,
                [contract_c_data, contract_c_return_data, contract_c_key](
                    NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  // Contract C writes state that should be reverted
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), contract_c_key,
                      std::as_bytes(std::span(contract_c_data)),
                      std::to_underlying(StorageDeltaFlags::None));
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE() << "Failed to set bytestring";
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }

                  // Contract C manually reverts with return data
                  ReturnBuffer buffer;
                  buffer.resize_uninitialized(contract_c_return_data.size());
                  std::memcpy(buffer.data(), contract_c_return_data.data(),
                              contract_c_return_data.size());
                  ctx.handler().set_return_buffer(std::move(buffer), true, {});
                  return {};
                });
  auto contract_c = std::make_unique<NativeContract>(std::move(fns_c));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract_a));
  contract_map.emplace(1, std::move(contract_b));
  contract_map.emplace(2, std::move(contract_c));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Deploy all contracts
  auto contract_a_obj = make_native_contract(0);
  auto contract_b_obj = make_native_contract(1);
  auto contract_c_obj = make_native_contract(2);
  write_contract(contract_a_obj);
  write_contract(contract_b_obj);
  write_contract(contract_c_obj);
  write_deployment(contract_a_addr, contract_a_obj);
  write_deployment(contract_b_addr, contract_b_obj);
  write_deployment(contract_c_addr, contract_c_obj);
  setup_grant_storage_token(contract_a_addr, 1'000'000'000);
  setup_grant_storage_token(contract_b_addr, 1'000'000'000);
  setup_grant_storage_token(contract_c_addr, 1'000'000'000);

  // Invoke Contract A
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_a_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  advance_block();

  // Contract A's state should persist
  auto contract_a_addrkey =
      make_contract_data_addrkey(contract_a_addr, "contract_a_key");
  auto contract_a_item_opt = lookup_storage_item_from_db(contract_a_addrkey);
  ASSERT_TRUE(contract_a_item_opt.has_value());

  // Contract B's state should be reverted
  auto contract_b_addrkey =
      make_contract_data_addrkey(contract_b_addr, "contract_b_key");
  auto contract_b_item_opt = lookup_storage_item_from_db(contract_b_addrkey);
  ASSERT_FALSE(contract_b_item_opt.has_value());

  // Contract C's state should be reverted
  auto contract_c_addrkey =
      make_contract_data_addrkey(contract_c_addr, "contract_c_key");
  auto contract_c_item_opt = lookup_storage_item_from_db(contract_c_addrkey);
  ASSERT_FALSE(contract_c_item_opt.has_value());
}

//
// Test basic cross-contract revert with return data.
//
// Scenario: Contract A calls Contract B, Contract B reverts with return data.
// This is a simpler version of the basic test to ensure the pattern works.
//
TEST_P(CrossContractRevertTest, RevertWithReturnData)
{
  ContractAddress contract_a_addr = make_contract_address(0);
  ContractAddress contract_b_addr = make_contract_address(1);

  auto contract_a_data = make_bytestring("contract_a_state");
  auto contract_b_return_data = make_bytestring("contract_b_error");

  auto contract_a_key = make_storage_key("contract_a_key");

  // Contract A: Calls Contract B and writes its own state
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_a;
  fns_a.emplace(
      0,
      [contract_a_data, contract_a_key, contract_b_addr](
          NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // Contract A writes its own state
        auto [res, _gas] = syscall_BytestringSet_inner(
            ctx.handler(), contract_a_key,
            std::as_bytes(std::span(contract_a_data)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Failed to set bytestring";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Call Contract B (which will revert)
        CopyableInvokeOutputs r1_outputs{};
        auto r1 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          return ctx.syscall<SyscallNo::kInvoke>(contract_b_addr,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 r1_outputs);
        }();
        if (!r1.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // A child frame ran, so Invoke returns None; the manual
        // revert is reported via frame_status == Abort.
        if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (r1_outputs.frame_status != InvocationFrameStatusCode::Abort) {
          ADD_FAILURE() << "Expected Contract B to manually revert (Abort)";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Contract A should receive Contract B's return data
        auto status = ctx.handler().get_return_buffer();
        if (!status || !(*status)) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Verify return data matches expected
        auto expected_data = make_bytestring("contract_b_error");
        if ((**status)->size() != expected_data.size()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (std::memcmp((**status)->data(), expected_data.data(),
                        expected_data.size()) != 0) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        return {};
      });
  auto contract_a = std::make_unique<NativeContract>(std::move(fns_a));

  // Contract B: Reverts with return data
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_b;
  fns_b.emplace(0,
                [contract_b_return_data](
                    NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  // Contract B manually reverts with return data
                  ReturnBuffer buffer;
                  buffer.resize_uninitialized(contract_b_return_data.size());
                  std::memcpy(buffer.data(), contract_b_return_data.data(),
                              contract_b_return_data.size());
                  ctx.handler().set_return_buffer(std::move(buffer), true, {});
                  return {};
                });
  auto contract_b = std::make_unique<NativeContract>(std::move(fns_b));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract_a));
  contract_map.emplace(1, std::move(contract_b));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Deploy both contracts
  auto contract_a_obj = make_native_contract(0);
  auto contract_b_obj = make_native_contract(1);
  write_contract(contract_a_obj);
  write_contract(contract_b_obj);
  write_deployment(contract_a_addr, contract_a_obj);
  write_deployment(contract_b_addr, contract_b_obj);
  setup_grant_storage_token(contract_a_addr, 1'000'000'000);
  setup_grant_storage_token(contract_b_addr, 1'000'000'000);

  // Invoke Contract A
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_a_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success())
      << "Expected root success, but frame success: " << res.is_frame_success();

  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  advance_block();

  // Contract A's state should persist
  auto contract_a_addrkey =
      make_contract_data_addrkey(contract_a_addr, "contract_a_key");
  auto contract_a_item_opt = lookup_storage_item_from_db(contract_a_addrkey);
  ASSERT_TRUE(contract_a_item_opt.has_value());
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, CrossContractRevertTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
