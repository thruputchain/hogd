#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// End-to-end test: run a transaction in proposal mode, capture the
// NondeterministicProposalData, then replay in validation mode and verify
// the results match.  Also test that tampered proposal data causes
// validation failure.
class ProposeValidateRoundtripTest : public ::testing::TestWithParam<TestDB>,
                                     public NativeContractTestFramework {
public:
  ProposeValidateRoundtripTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  void SetUp() override
  {
    // Deploy two native contracts
    write_contract(native_contract_a_xdr_);
    write_deployment(kAddrA, native_contract_a_xdr_);
    setup_grant_storage_token(kAddrA, 1'000'000'000);

    write_contract(native_contract_b_xdr_);
    write_deployment(kAddrB, native_contract_b_xdr_);
    setup_grant_storage_token(kAddrB, 1'000'000'000);
  }

  static constexpr ContractAddress kAddrA = make_contract_address(0);
  static constexpr ContractAddress kAddrB = make_contract_address(1);

  std::shared_ptr<Contract> native_contract_a_xdr_ = make_native_contract(0);
  std::shared_ptr<Contract> native_contract_b_xdr_ = make_native_contract(1);
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// Contract A method 0: try_invoke Contract B method 0, then try_invoke
// Contract B method 1.  Both succeed, producing 2 nondeterministic bits
// (both true).  Nondeterministic bits are only logged when the inner frame
// succeeds (deterministic errors skip lock acquisition entirely).
//
// Contract B method 0: writes bytestring "hello" to storage.
// Contract B method 1: writes bytestring "world" to storage.
TEST_P(ProposeValidateRoundtripTest, SuccessfulRoundtrip)
{
  auto native_contract_a = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             // try_invoke B method 0 — should succeed
             auto r0 = [&]() {
               RuntimeMemoryLength output_return_buffer_len = 0;
               return ctx.syscall<SyscallNo::kInvoke>(
                   kAddrB, 0, ctx.runtime().get_available_gas(), Calldata{},
                   std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke),
                   output_return_buffer_len);
             }();
             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // try_invoke B method 1 — should also succeed
             auto r1 = [&]() {
               RuntimeMemoryLength output_return_buffer_len = 0;
               return ctx.syscall<SyscallNo::kInvoke>(
                   kAddrB, 1, ctx.runtime().get_available_gas(), Calldata{},
                   std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke),
                   output_return_buffer_len);
             }();
             if (!r1.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Write something to prove we survived both try_invokes
             auto key = make_storage_key("result");
             auto [ws, _wg] = syscall_BytestringSet_inner(
                 ctx.handler(), key,
                 std::as_bytes(std::span(std::string_view("ok"))),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
             if (!ws.has_value() ||
                 ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
      });

  auto native_contract_b = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("b_data0");
             auto [ws, _wg] = syscall_BytestringSet_inner(
                 ctx.handler(), key,
                 std::as_bytes(std::span(std::string_view("hello"))),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
             if (!ws.has_value() ||
                 ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
          {1,
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("b_data1");
             auto [ws, _wg] = syscall_BytestringSet_inner(
                 ctx.handler(), key,
                 std::as_bytes(std::span(std::string_view("world"))),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
             if (!ws.has_value() ||
                 ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_a));
  contract_map.emplace(1, std::move(native_contract_b));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // === Phase 1: Proposal mode ===
  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx, *native_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success())
        << "Proposal mode root_invoke should succeed";

    proposal_data = handler.get_proposal_data();

    // The transaction had 2 try_invokes, so there should be 2 bits
    EXPECT_EQ(proposal_data.nondeterministic_bit_count, 2U);

    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }

  // Discard the block and re-open at the same dbtip so validation sees
  // the same snapshot as the proposal.
  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // === Phase 2: Validation mode with correct proposal data ===
  {
    auto handler = make_new_system_call_handler_validation(tx, *native_context,
                                                           proposal_data);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success())
        << "Validation with correct proposal data should succeed";

    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }
}

// Test that validation fails when the proposal data has extra unconsumed
// nondeterministic bits (entropy grinding attack vector).
TEST_P(ProposeValidateRoundtripTest, ExtraBitsCauseValidationFailure)
{
  auto native_contract_a = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             // Single try_invoke that succeeds: produces 1 bit
             auto r0 = [&]() {
               RuntimeMemoryLength output_return_buffer_len = 0;
               return ctx.syscall<SyscallNo::kInvoke>(
                   kAddrB, 0, ctx.runtime().get_available_gas(), Calldata{},
                   std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke),
                   output_return_buffer_len);
             }();
             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
      });

  auto native_contract_b = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("b_data");
             auto [ws, _wg] = syscall_BytestringSet_inner(
                 ctx.handler(), key,
                 std::as_bytes(std::span(std::string_view("hello"))),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
             if (!ws.has_value() ||
                 ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_a));
  contract_map.emplace(1, std::move(native_contract_b));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // === Phase 1: Proposal ===
  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx, *native_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    proposal_data = handler.get_proposal_data();
    EXPECT_EQ(proposal_data.nondeterministic_bit_count, 1U);
    ASSERT_TRUE(handler.abort_transaction().has_value());
  }

  // === Phase 2: Tamper — add extra nondeterministic bits ===
  {
    auto tampered = proposal_data;
    // Add an extra bit: now bit_count=2 but execution only consumes 1
    tampered.nondeterministic_bit_count = 2;

    auto handler = make_new_system_call_handler_validation(tx, *native_context,
                                                           std::move(tampered));
    auto res = handler.root_invoke();
    // check_all_consumed() should fail because 2 bits were supplied but
    // only 1 was consumed.
    EXPECT_FALSE(res.is_root_success())
        << "Validation should fail with extra unconsumed bits";
    ASSERT_TRUE(handler.abort_transaction().has_value());
  }
}

// Test that validation fails when proposal data has a flipped bit.
// Contract A does two try_invokes, but only does the second if the first
// succeeds.  In proposal mode both succeed, producing 2 bits (both true).
// Flipping bit 0 to false causes the validator to skip the second
// try_invoke, consuming only 1 of 2 bits → check_all_consumed() fails.
TEST_P(ProposeValidateRoundtripTest, FlippedBitsCauseValidationFailure)
{
  auto native_contract_a = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             // try_invoke B method 0
             auto r0 = [&]() {
               RuntimeMemoryLength output_return_buffer_len = 0;
               return ctx.syscall<SyscallNo::kInvoke>(
                   kAddrB, 0, ctx.runtime().get_available_gas(), Calldata{},
                   std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke),
                   output_return_buffer_len);
             }();
             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Only do the second try_invoke if the first succeeded
             if (r0.value() == SyscallErrorCode::kSyscallErrorCodeNone) {
               auto r1 = [&]() {
                 RuntimeMemoryLength output_return_buffer_len = 0;
                 return ctx.syscall<SyscallNo::kInvoke>(
                     kAddrB, 1, ctx.runtime().get_available_gas(), Calldata{},
                     std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke),
                     output_return_buffer_len);
               }();
               if (!r1.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
             }

             return {};
           }},
      });

  auto native_contract_b = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("b_data0");
             auto [ws, _wg] = syscall_BytestringSet_inner(
                 ctx.handler(), key,
                 std::as_bytes(std::span(std::string_view("hello"))),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
             if (!ws.has_value() ||
                 ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
          {1,
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("b_data1");
             auto [ws, _wg] = syscall_BytestringSet_inner(
                 ctx.handler(), key,
                 std::as_bytes(std::span(std::string_view("world"))),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
             if (!ws.has_value() ||
                 ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_a));
  contract_map.emplace(1, std::move(native_contract_b));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // === Phase 1: Proposal ===
  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx, *native_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    proposal_data = handler.get_proposal_data();
    ASSERT_EQ(proposal_data.nondeterministic_bit_count, 2U);
    ASSERT_TRUE(handler.abort_transaction().has_value());
  }

  // === Phase 2: Tamper — flip bit 0 from true to false ===
  // With bit 0 = false, the validator treats the first try_invoke as
  // failed, so contract A skips the second try_invoke. Only 1 of the
  // 2 nondeterministic bits is consumed → check_all_consumed() fails.
  {
    auto tampered = proposal_data;
    tampered.nondeterministic_bits[0] ^= 1;

    discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

    auto handler = make_new_system_call_handler_validation(tx, *native_context,
                                                           std::move(tampered));
    auto res = handler.root_invoke();
    // The root invoke succeeds (contract A returns Ok), but
    // check_final_validation_data detects the unconsumed bit.
    if (res.is_root_success()) {
      auto commit_res = handler.try_commit_transaction();
      EXPECT_NE(commit_res, ExecResult::ExecResultOK)
          << "Validation with flipped bits should not commit successfully";
    }
    else {
      // Root invoke failed — also acceptable
      ASSERT_TRUE(handler.abort_transaction().has_value());
    }
  }
}

// Test that validation fails when the proposal is structurally malformed
// (e.g. bit count doesn't match byte count).
TEST_P(ProposeValidateRoundtripTest, MalformedProposalRejectedEarly)
{
  auto native_contract_a = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](NativeSyscallContext & /*unused*/) -> InvokeStatus<void> {
             // Simple no-op — just return success
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_a));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // Create malformed proposal: claim 10 bits but provide 0 bytes
  NondeterministicProposalData malformed;
  malformed.nondeterministic_bit_count = 10;
  malformed.nondeterministic_bits.clear(); // 0 bytes, but claims 10 bits

  auto handler = make_new_system_call_handler_validation(tx, *native_context,
                                                         std::move(malformed));
  auto res = handler.root_invoke();
  // validate() should reject this in perform_initial_validation
  EXPECT_FALSE(res.is_root_success())
      << "Malformed proposal (bit count/byte count mismatch) should be "
         "rejected";
  ASSERT_TRUE(handler.abort_transaction().has_value());
}

// Test that validation fails when there are padding bits set in the last
// byte (entropy grinding vector).
TEST_P(ProposeValidateRoundtripTest, PaddingBitEntropyGrinding)
{
  auto native_contract_a = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](NativeSyscallContext & /*unused*/) -> InvokeStatus<void> {
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_a));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // Create proposal with 1 bit but set high bits in the byte (padding
  // should be zero to prevent entropy grinding).
  NondeterministicProposalData grinding;
  grinding.nondeterministic_bit_count = 1;
  grinding.nondeterministic_bits = {
      0xFF}; // high 7 bits are padding, should be 0

  auto handler = make_new_system_call_handler_validation(tx, *native_context,
                                                         std::move(grinding));
  auto res = handler.root_invoke();
  EXPECT_FALSE(res.is_root_success())
      << "Padding bits set in last byte should be rejected (entropy "
         "grinding)";
  ASSERT_TRUE(handler.abort_transaction().has_value());
}

// ===================================================================
// RCQ Validation Mode Tests
// ===================================================================
//
// These tests exercise the proposer/validator pipeline for
// RetrocausalQueue operations.  The most critical code path is the
// concurrent-entry observation replay in item_view.cc:326-373 and the
// consumed_but_not_inserted reconciliation in
// refcounted_delta_variant.cc:316-360.

// Single TX inserts three entries.  No nondeterministic bits (insert
// is deterministic).  Validates the basic RCQ-through-validation
// pipeline.
TEST_P(ProposeValidateRoundtripTest, RCQInsertRoundtrip)
{
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             for (uint8_t val : {1, 2, 3}) {
               auto res = ctx.handler().retrocausal_queue_insert(
                   key, std::as_bytes(std::span(&val, 1)));
               if (!res.has_value()) {
                 return res.error().to_invoke_error();
               }
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // === Proposal ===
  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx, *native_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    proposal_data = handler.get_proposal_data();
    // Insert is fully deterministic — no nondeterministic bits expected
    EXPECT_EQ(proposal_data.nondeterministic_bit_count, 0U);
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // === Validation ===
  {
    auto handler = make_new_system_call_handler_validation(tx, *native_context,
                                                           proposal_data);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success())
        << "Validation of RCQ insert should succeed";
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }
}

// Setup: insert entries in block 1.  Block 2: consume past entries.
// Past-block consume is deterministic (entries are in the snapshot),
// so no nondeterministic bits from RCQ.
TEST_P(ProposeValidateRoundtripTest, RCQConsumePastEntriesRoundtrip)
{
  std::vector<uint8_t> consumed_values;

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, // Insert
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             for (uint8_t val : {1, 2, 3}) {
               auto res = ctx.handler().retrocausal_queue_insert(
                   key, std::as_bytes(std::span(&val, 1)));
               if (!res.has_value()) {
                 return res.error().to_invoke_error();
               }
             }
             return {};
           }},
          {1, // Consume 2
           [&consumed_values](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             consumed_values.clear();
             for (int i = 0; i < 2; i++) {
               auto res =
                   ctx.handler().retrocausal_queue_consume(key, false);
               if (!res.has_value()) {
                 return res.error().to_invoke_error();
               }
               if (!res.value().has_value()) {
                 break;
               }
               auto const &entry = res.value().value();
               if (entry.size() == 1) {
                 consumed_values.push_back(entry[0]);
               }
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Block 1: insert entries {1, 2, 3}
  {
    Transaction tx_insert{
        .invocation = Invocation{.invoked_address = kAddrA,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
    };
    auto handler = make_new_system_call_handler(tx_insert, *native_context);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  }
  advance_block();

  // Block 2: consume 2 entries
  Transaction tx_consume{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(1),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // === Proposal ===
  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx_consume, *native_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    proposal_data = handler.get_proposal_data();
    // Past-block consume doesn't produce nondeterministic bits
    EXPECT_EQ(proposal_data.nondeterministic_bit_count, 0U);
    EXPECT_EQ(consumed_values, (std::vector<uint8_t>{1, 2}));
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // === Validation ===
  {
    auto handler = make_new_system_call_handler_validation(
        tx_consume, *native_context, proposal_data);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success())
        << "Validation of past-block consume should succeed";
    EXPECT_EQ(consumed_values, (std::vector<uint8_t>{1, 2}));
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }
}

// CRITICAL TEST: Two TXs in same block.  TX_A inserts, TX_B consumes
// a concurrent entry inserted by TX_A.  This produces nondeterministic
// bits (entry existence) and queue entry observations.
TEST_P(ProposeValidateRoundtripTest, RCQConcurrentEntryRoundtrip)
{
  std::vector<uint8_t> consumed_values;

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, // Insert single entry {10}
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             uint8_t val = 10;
             auto res = ctx.handler().retrocausal_queue_insert(
                 key, std::as_bytes(std::span(&val, 1)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return {};
           }},
          {1, // Consume 1 entry
           [&consumed_values](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             consumed_values.clear();
             auto res = ctx.handler().retrocausal_queue_consume(key, false);
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             if (res.value().has_value()) {
               auto const &entry = res.value().value();
               if (entry.size() == 1) {
                 consumed_values.push_back(entry[0]);
               }
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx_insert{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };
  Transaction tx_consume{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(1),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // === Proposal: TX_A inserts, commits, then TX_B consumes concurrent ===
  NondeterministicProposalData insert_proposal_data;
  NondeterministicProposalData consume_proposal_data;
  {
    // TX_A: insert {10} and commit
    auto handler_a =
        make_new_system_call_handler(tx_insert, *native_context);
    ASSERT_TRUE(handler_a.root_invoke().is_root_success());
    insert_proposal_data = handler_a.get_proposal_data();
    ASSERT_EQ(handler_a.try_commit_transaction(), ExecResult::ExecResultOK);

    // TX_B: consume — should see {10} as a concurrent entry
    auto handler_b =
        make_new_system_call_handler(tx_consume, *native_context);
    ASSERT_TRUE(handler_b.root_invoke().is_root_success());
    consume_proposal_data = handler_b.get_proposal_data();

    // TX_B should have consumed concurrent entry {10}:
    //   1 nondeterministic bit (entry exists = true)
    //   1 queue entry observation ({10})
    EXPECT_EQ(consume_proposal_data.nondeterministic_bit_count, 1U);
    EXPECT_EQ(consume_proposal_data.observed_queue_entry_sequence.size(), 1U);
    EXPECT_EQ(consumed_values, (std::vector<uint8_t>{10}));

    ASSERT_EQ(handler_b.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  // Capture proposal state for comparison
  DbTip proposal_tip;
  if (GetParam() == TestDB::kHogDB) {
    advance_block();
    proposal_tip = get_db_tip();
    // Rewind: discard the committed block so we can re-validate
    discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
  }
  else {
    discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
  }

  // === Validation: replay both TXs with captured proposal data ===
  {
    // TX_A: insert {10} — same as proposal
    auto handler_a = make_new_system_call_handler_validation(
        tx_insert, *native_context, insert_proposal_data);
    ASSERT_TRUE(handler_a.root_invoke().is_root_success());
    ASSERT_EQ(handler_a.try_commit_transaction(), ExecResult::ExecResultOK);

    // TX_B: consume — validation replays: bit=true, observation={10}
    auto handler_b = make_new_system_call_handler_validation(
        tx_consume, *native_context, consume_proposal_data);
    ASSERT_TRUE(handler_b.root_invoke().is_root_success())
        << "Validation of concurrent entry consumption should succeed";
    EXPECT_EQ(consumed_values, (std::vector<uint8_t>{10}));
    ASSERT_EQ(handler_b.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  // Verify state agreement
  if (GetParam() == TestDB::kHogDB) {
    advance_block();
    auto validation_tip = get_db_tip();
    EXPECT_EQ(proposal_tip.hash, validation_tip.hash)
        << "Proposal and validation trie root hashes must match";
  }
}

// TX tries to consume from a queue where past entries are exhausted
// and no concurrent entries exist.  The nondeterministic bit is false
// (no concurrent entry found).
TEST_P(ProposeValidateRoundtripTest, RCQConsumeEmptyRoundtrip)
{
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, // Insert single entry
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             uint8_t val = 1;
             auto res = ctx.handler().retrocausal_queue_insert(
                 key, std::as_bytes(std::span(&val, 1)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return {};
           }},
          {1, // Consume 2 entries (second will be empty)
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             for (int i = 0; i < 2; i++) {
               auto res = ctx.handler().retrocausal_queue_consume(key, false);
               if (!res.has_value()) {
                 return res.error().to_invoke_error();
               }
               // First consume returns entry, second returns empty
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Block 1: insert 1 entry
  {
    Transaction tx_insert{
        .invocation = Invocation{.invoked_address = kAddrA,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
    };
    auto handler = make_new_system_call_handler(tx_insert, *native_context);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  }
  advance_block();

  // Block 2: consume 2 entries (only 1 exists)
  Transaction tx_consume{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(1),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // === Proposal ===
  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx_consume, *native_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    proposal_data = handler.get_proposal_data();
    // First consume: past block entry (deterministic, no bit)
    // Second consume: no past entries left, no self entries, tries concurrent
    //   -> bit=false (no concurrent entry), no observation
    EXPECT_EQ(proposal_data.nondeterministic_bit_count, 1U);
    EXPECT_EQ(proposal_data.observed_queue_entry_sequence.size(), 0U);
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // === Validation ===
  {
    auto handler = make_new_system_call_handler_validation(
        tx_consume, *native_context, proposal_data);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success())
        << "Validation with bit=false (empty concurrent) should succeed";
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }
}

// Single TX inserts then consumes its own entries (self-entries via
// get_next_self_entry, deterministic) plus checks concurrent entries.
TEST_P(ProposeValidateRoundtripTest, RCQInsertConsumeSameTxRoundtrip)
{
  std::vector<uint8_t> consumed_values;

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, // Insert 2 entries, then consume 3 (2 self + 1 concurrent miss)
           [&consumed_values](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             consumed_values.clear();

             // Insert {10, 20}
             for (uint8_t val : {10, 20}) {
               auto r = ctx.handler().retrocausal_queue_insert(
                   key, std::as_bytes(std::span(&val, 1)));
               if (!r.has_value()) {
                 return r.error().to_invoke_error();
               }
             }

             // Consume 3 (2 self-entries + 1 miss on concurrent)
             for (int i = 0; i < 3; i++) {
               auto r = ctx.handler().retrocausal_queue_consume(key, false);
               if (!r.has_value()) {
                 return r.error().to_invoke_error();
               }
               if (r.value().has_value()) {
                 auto const &entry = r.value().value();
                 if (entry.size() == 1) {
                   consumed_values.push_back(entry[0]);
                 }
               }
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // === Proposal ===
  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx, *native_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    proposal_data = handler.get_proposal_data();
    // First 2 consumes hit self-entries (deterministic)
    // Third consume: no past, no self, tries concurrent -> bit=false
    EXPECT_EQ(proposal_data.nondeterministic_bit_count, 1U);
    EXPECT_EQ(consumed_values, (std::vector<uint8_t>{10, 20}));
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // === Validation ===
  {
    auto handler = make_new_system_call_handler_validation(tx, *native_context,
                                                           proposal_data);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success())
        << "Validation of self-consume + concurrent miss should succeed";
    EXPECT_EQ(consumed_values, (std::vector<uint8_t>{10, 20}));
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }
}

// ===================================================================
// Adversarial / Negative RCQ Validation Tests
// ===================================================================

// Tamper: add an extra, unconsumed queue entry observation.
// check_all_consumed() should detect the extra observation.
TEST_P(ProposeValidateRoundtripTest, RCQExtraObservationsCauseFailure)
{
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, // Insert {10}
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             uint8_t val = 10;
             auto res = ctx.handler().retrocausal_queue_insert(
                 key, std::as_bytes(std::span(&val, 1)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return {};
           }},
          {1, // Consume 1
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             auto res = ctx.handler().retrocausal_queue_consume(key, false);
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx_insert{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };
  Transaction tx_consume{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(1),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // Proposal: insert + commit, then consume + capture
  NondeterministicProposalData insert_proposal_data;
  NondeterministicProposalData consume_proposal_data;
  {
    auto handler_a =
        make_new_system_call_handler(tx_insert, *native_context);
    ASSERT_TRUE(handler_a.root_invoke().is_root_success());
    insert_proposal_data = handler_a.get_proposal_data();
    ASSERT_EQ(handler_a.try_commit_transaction(), ExecResult::ExecResultOK);

    auto handler_b =
        make_new_system_call_handler(tx_consume, *native_context);
    ASSERT_TRUE(handler_b.root_invoke().is_root_success());
    consume_proposal_data = handler_b.get_proposal_data();
    ASSERT_TRUE(handler_b.abort_transaction().has_value());
  }

  // Tamper: add an extra observation
  auto tampered = consume_proposal_data;
  QueueEntry extra_entry;
  extra_entry.push_back(99);
  tampered.observed_queue_entry_sequence.push_back(extra_entry);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Insert TX first
  {
    auto handler_a = make_new_system_call_handler_validation(
        tx_insert, *native_context, insert_proposal_data);
    ASSERT_TRUE(handler_a.root_invoke().is_root_success());
    ASSERT_EQ(handler_a.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  // Consume TX with tampered data — should fail validation
  {
    auto handler_b = make_new_system_call_handler_validation(
        tx_consume, *native_context, std::move(tampered));
    auto res = handler_b.root_invoke();
    // check_all_consumed() should detect extra unconsumed observation
    EXPECT_FALSE(res.is_root_success())
        << "Extra queue entry observations should cause validation failure";
    ASSERT_TRUE(handler_b.abort_transaction().has_value());
  }
}

// Tamper: remove the queue entry observation.
// consume_queue_entry_observation returns nullopt → kSystemValidationFailure.
TEST_P(ProposeValidateRoundtripTest, RCQMissingObservationCausesFailure)
{
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, // Insert {10}
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             uint8_t val = 10;
             auto res = ctx.handler().retrocausal_queue_insert(
                 key, std::as_bytes(std::span(&val, 1)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return {};
           }},
          {1, // Consume 1
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             auto res = ctx.handler().retrocausal_queue_consume(key, false);
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx_insert{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };
  Transaction tx_consume{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(1),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // Proposal
  NondeterministicProposalData insert_proposal_data;
  NondeterministicProposalData consume_proposal_data;
  {
    auto handler_a =
        make_new_system_call_handler(tx_insert, *native_context);
    ASSERT_TRUE(handler_a.root_invoke().is_root_success());
    insert_proposal_data = handler_a.get_proposal_data();
    ASSERT_EQ(handler_a.try_commit_transaction(), ExecResult::ExecResultOK);

    auto handler_b =
        make_new_system_call_handler(tx_consume, *native_context);
    ASSERT_TRUE(handler_b.root_invoke().is_root_success());
    consume_proposal_data = handler_b.get_proposal_data();
    ASSERT_EQ(consume_proposal_data.observed_queue_entry_sequence.size(), 1U);
    ASSERT_TRUE(handler_b.abort_transaction().has_value());
  }

  // Tamper: remove the observation (but keep the bit saying entry exists)
  auto tampered = consume_proposal_data;
  tampered.observed_queue_entry_sequence.clear();

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto handler_a = make_new_system_call_handler_validation(
        tx_insert, *native_context, insert_proposal_data);
    ASSERT_TRUE(handler_a.root_invoke().is_root_success());
    ASSERT_EQ(handler_a.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  // Validation should fail — bit says entry exists but no observation to consume
  {
    auto handler_b = make_new_system_call_handler_validation(
        tx_consume, *native_context, std::move(tampered));
    auto res = handler_b.root_invoke();
    EXPECT_FALSE(res.is_root_success())
        << "Missing queue entry observation should cause validation failure";
    ASSERT_TRUE(handler_b.abort_transaction().has_value());
  }
}

// Tamper: flip existence bit from true to false.
// Validator skips observation consumption → unconsumed observation →
// check_all_consumed() fails.
TEST_P(ProposeValidateRoundtripTest, RCQExistenceBitFlippedToFalse)
{
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, // Insert {10}
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             uint8_t val = 10;
             auto res = ctx.handler().retrocausal_queue_insert(
                 key, std::as_bytes(std::span(&val, 1)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return {};
           }},
          {1, // Consume 1
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             auto res = ctx.handler().retrocausal_queue_consume(key, false);
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx_insert{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };
  Transaction tx_consume{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(1),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // Proposal
  NondeterministicProposalData insert_proposal_data;
  NondeterministicProposalData consume_proposal_data;
  {
    auto handler_a =
        make_new_system_call_handler(tx_insert, *native_context);
    ASSERT_TRUE(handler_a.root_invoke().is_root_success());
    insert_proposal_data = handler_a.get_proposal_data();
    ASSERT_EQ(handler_a.try_commit_transaction(), ExecResult::ExecResultOK);

    auto handler_b =
        make_new_system_call_handler(tx_consume, *native_context);
    ASSERT_TRUE(handler_b.root_invoke().is_root_success());
    consume_proposal_data = handler_b.get_proposal_data();
    ASSERT_EQ(consume_proposal_data.nondeterministic_bit_count, 1U);
    ASSERT_TRUE(handler_b.abort_transaction().has_value());
  }

  // Tamper: flip bit 0 from true to false
  auto tampered = consume_proposal_data;
  tampered.nondeterministic_bits[0] ^= 1; // flip bit 0

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto handler_a = make_new_system_call_handler_validation(
        tx_insert, *native_context, insert_proposal_data);
    ASSERT_TRUE(handler_a.root_invoke().is_root_success());
    ASSERT_EQ(handler_a.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  {
    auto handler_b = make_new_system_call_handler_validation(
        tx_consume, *native_context, std::move(tampered));
    auto res = handler_b.root_invoke();
    // bit=false → validator returns nullopt, skips observation
    // → unconsumed observation → check_all_consumed() fails
    EXPECT_FALSE(res.is_root_success())
        << "Flipped existence bit (true→false) should cause validation failure";
    ASSERT_TRUE(handler_b.abort_transaction().has_value());
  }
}

// Tamper: flip existence bit from false to true.
// Validator tries to consume an observation that doesn't exist.
TEST_P(ProposeValidateRoundtripTest, RCQExistenceBitFlippedToTrue)
{
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, // Insert {10}
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             uint8_t val = 10;
             auto res = ctx.handler().retrocausal_queue_insert(
                 key, std::as_bytes(std::span(&val, 1)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return {};
           }},
          {1, // Consume 2 entries (1 past + 1 concurrent miss)
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             for (int i = 0; i < 2; i++) {
               auto res = ctx.handler().retrocausal_queue_consume(key, false);
               if (!res.has_value()) {
                 return res.error().to_invoke_error();
               }
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Block 1: insert 1 entry
  {
    Transaction tx_insert{
        .invocation = Invocation{.invoked_address = kAddrA,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
    };
    auto handler = make_new_system_call_handler(tx_insert, *native_context);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  }
  advance_block();

  // Block 2: consume 2 entries (1 past, then concurrent=empty)
  Transaction tx_consume{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(1),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // Proposal
  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx_consume, *native_context);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    proposal_data = handler.get_proposal_data();
    // 1 bit (false — no concurrent entry), 0 observations
    ASSERT_EQ(proposal_data.nondeterministic_bit_count, 1U);
    ASSERT_EQ(proposal_data.observed_queue_entry_sequence.size(), 0U);
    ASSERT_TRUE(handler.abort_transaction().has_value());
  }

  // Tamper: flip bit 0 from false to true
  auto tampered = proposal_data;
  tampered.nondeterministic_bits[0] ^= 1; // now true

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Validation: bit=true → tries to consume observation, but none exists
  {
    auto handler = make_new_system_call_handler_validation(
        tx_consume, *native_context, std::move(tampered));
    auto res = handler.root_invoke();
    EXPECT_FALSE(res.is_root_success())
        << "Flipped existence bit (false→true) with no observation should "
           "cause validation failure";
    ASSERT_TRUE(handler.abort_transaction().has_value());
  }
}

// ===================================================================
// Impossible Commit Set Tests
//
// These test scenarios where a malicious proposer fabricates
// internally consistent-looking proposal data that nonetheless
// describes an impossible execution.  Individual TX commits succeed
// (they only track deltas), but block-level finalization detects the
// inconsistency in write_committed_change.
//
// These tests require kHogDB because the check is in the hogdb layer
// (retrocausal_queue_lock.cc write_committed_change).
// ===================================================================

// Proposer claims TX_B consumed entry {99} but TX_A only inserted {10}.
// Per-TX commits succeed, but block finalization detects the mismatch:
// consumed_set has {99}, added_entries has {10} → no match → error.
TEST_P(ProposeValidateRoundtripTest, RCQFabricatedObservationValue)
{
  if (GetParam() != TestDB::kHogDB) {
    GTEST_SKIP() << "write_committed_change check requires kHogDB";
  }

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, // Insert {10}
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             uint8_t val = 10;
             auto res = ctx.handler().retrocausal_queue_insert(
                 key, std::as_bytes(std::span(&val, 1)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return {};
           }},
          {1, // Consume 1
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             auto res = ctx.handler().retrocausal_queue_consume(key, false);
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx_insert{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };
  Transaction tx_consume{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(1),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // Proposal: TX_A inserts {10}, TX_B consumes concurrent {10}
  NondeterministicProposalData insert_proposal_data;
  NondeterministicProposalData consume_proposal_data;
  {
    auto handler_a =
        make_new_system_call_handler(tx_insert, *native_context);
    ASSERT_TRUE(handler_a.root_invoke().is_root_success());
    insert_proposal_data = handler_a.get_proposal_data();
    ASSERT_EQ(handler_a.try_commit_transaction(), ExecResult::ExecResultOK);

    auto handler_b =
        make_new_system_call_handler(tx_consume, *native_context);
    ASSERT_TRUE(handler_b.root_invoke().is_root_success());
    consume_proposal_data = handler_b.get_proposal_data();
    ASSERT_EQ(consume_proposal_data.observed_queue_entry_sequence.size(), 1U);
    ASSERT_EQ(handler_b.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  // Tamper: change observation from {10} to {99}
  auto tampered = consume_proposal_data;
  tampered.observed_queue_entry_sequence[0].clear();
  tampered.observed_queue_entry_sequence[0].push_back(99);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Validation: both TXs execute and commit successfully (per-TX level)
  {
    auto handler_a = make_new_system_call_handler_validation(
        tx_insert, *native_context, insert_proposal_data);
    ASSERT_TRUE(handler_a.root_invoke().is_root_success());
    ASSERT_EQ(handler_a.try_commit_transaction(), ExecResult::ExecResultOK);

    auto handler_b = make_new_system_call_handler_validation(
        tx_consume, *native_context, std::move(tampered));
    ASSERT_TRUE(handler_b.root_invoke().is_root_success());
    ASSERT_EQ(handler_b.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  // Block finalization should fail: consumed_set={99} vs added_entries={10}
  EXPECT_THROW(advance_block(), std::runtime_error)
      << "Block with fabricated observation value should fail finalization";
}

// Proposer claims TX_B consumed a concurrent entry from TX_A, but TX_A
// is not included in the validation set (as if it was aborted).
// TX_B's per-TX commit succeeds, but block finalization detects that the
// consumed entry {10} has no matching insert.
TEST_P(ProposeValidateRoundtripTest, RCQAbortedInserterEntryConsumed)
{
  if (GetParam() != TestDB::kHogDB) {
    GTEST_SKIP() << "write_committed_change check requires kHogDB";
  }

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, // Insert {10}
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             uint8_t val = 10;
             auto res = ctx.handler().retrocausal_queue_insert(
                 key, std::as_bytes(std::span(&val, 1)));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return {};
           }},
          {1, // Consume 1
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             auto res = ctx.handler().retrocausal_queue_consume(key, false);
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx_insert{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };
  Transaction tx_consume{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(1),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // Proposal: TX_A inserts {10}, TX_B consumes concurrent {10}
  NondeterministicProposalData consume_proposal_data;
  {
    auto handler_a =
        make_new_system_call_handler(tx_insert, *native_context);
    ASSERT_TRUE(handler_a.root_invoke().is_root_success());
    ASSERT_EQ(handler_a.try_commit_transaction(), ExecResult::ExecResultOK);

    auto handler_b =
        make_new_system_call_handler(tx_consume, *native_context);
    ASSERT_TRUE(handler_b.root_invoke().is_root_success());
    consume_proposal_data = handler_b.get_proposal_data();
    ASSERT_EQ(handler_b.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Validation: only TX_B runs (TX_A is "aborted" — not included)
  // TX_B replays with bit=true, observation={10}, but nobody inserts {10}
  {
    auto handler_b = make_new_system_call_handler_validation(
        tx_consume, *native_context, std::move(consume_proposal_data));
    ASSERT_TRUE(handler_b.root_invoke().is_root_success());
    ASSERT_EQ(handler_b.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  // Block finalization: consumed_set={10} but added_entries={} → mismatch
  EXPECT_THROW(advance_block(), std::runtime_error)
      << "Block with consumed entry from missing inserter should fail";
}

// Proposer claims consumer consumed 2 concurrent entries, but only 1
// inserter committed (providing 1 entry).  The second consumed entry
// has no matching insert.
TEST_P(ProposeValidateRoundtripTest, RCQMoreConsumedThanInserted)
{
  if (GetParam() != TestDB::kHogDB) {
    GTEST_SKIP() << "write_committed_change check requires kHogDB";
  }

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, // Insert: calldata[0] is the value
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto const &calldata = ctx.handler().get_calldata();
             auto key = make_storage_key("rcq0");
             auto res = ctx.handler().retrocausal_queue_insert(
                 key,
                 std::as_bytes(std::span(calldata.data(), calldata.size())));
             if (!res.has_value()) {
               return res.error().to_invoke_error();
             }
             return {};
           }},
          {1, // Consume 2
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             for (int i = 0; i < 2; i++) {
               auto res = ctx.handler().retrocausal_queue_consume(key, false);
               if (!res.has_value()) {
                 return res.error().to_invoke_error();
               }
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx_insert_a{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = Calldata({10})},
  };
  Transaction tx_insert_b{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = Calldata({20})},
  };
  Transaction tx_consume{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(1),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // Proposal: TX_A inserts {10}, TX_B inserts {20}, TX_C consumes 2
  NondeterministicProposalData insert_a_proposal;
  NondeterministicProposalData consume_proposal;
  {
    auto handler_a =
        make_new_system_call_handler(tx_insert_a, *native_context);
    ASSERT_TRUE(handler_a.root_invoke().is_root_success());
    insert_a_proposal = handler_a.get_proposal_data();
    ASSERT_EQ(handler_a.try_commit_transaction(), ExecResult::ExecResultOK);

    auto handler_b =
        make_new_system_call_handler(tx_insert_b, *native_context);
    ASSERT_TRUE(handler_b.root_invoke().is_root_success());
    ASSERT_EQ(handler_b.try_commit_transaction(), ExecResult::ExecResultOK);

    auto handler_c =
        make_new_system_call_handler(tx_consume, *native_context);
    ASSERT_TRUE(handler_c.root_invoke().is_root_success());
    consume_proposal = handler_c.get_proposal_data();
    // Should have consumed 2 concurrent entries: {10} and {20}
    ASSERT_EQ(consume_proposal.observed_queue_entry_sequence.size(), 2U);
    ASSERT_EQ(handler_c.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Validation: only TX_A and TX_C run (TX_B "aborted" — not included)
  // TX_C replays with 2 observations ({10}, {20}) but only {10} is inserted
  {
    auto handler_a = make_new_system_call_handler_validation(
        tx_insert_a, *native_context, insert_a_proposal);
    ASSERT_TRUE(handler_a.root_invoke().is_root_success());
    ASSERT_EQ(handler_a.try_commit_transaction(), ExecResult::ExecResultOK);

    auto handler_c = make_new_system_call_handler_validation(
        tx_consume, *native_context, std::move(consume_proposal));
    ASSERT_TRUE(handler_c.root_invoke().is_root_success());
    ASSERT_EQ(handler_c.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  // Block finalization: consumed_set={10,20}, added_entries={10}
  // {10} matches, {20} remains in consumed_set → error
  EXPECT_THROW(advance_block(), std::runtime_error)
      << "Block with more consumed than inserted should fail finalization";
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, ProposeValidateRoundtripTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
