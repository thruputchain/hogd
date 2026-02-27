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

  // --- RCQ test helpers ---

  // Calldata-driven RCQ contract:
  //   method 0: each calldata byte → one queue entry
  //   method 1: consume calldata[0] entries (default 1)
  std::unique_ptr<RuntimeContext> make_rcq_context()
  {
    auto native_contract = std::make_unique<NativeContract>(
        std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
            {0,
             [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               auto const &cd = ctx.handler().get_calldata();
               auto key = make_storage_key("rcq0");
               for (uint8_t val : cd) {
                 auto res = ctx.handler().retrocausal_queue_insert(
                     key, std::as_bytes(std::span(&val, 1)));
                 if (!res.has_value())
                   return res.error().to_invoke_error();
               }
               return {};
             }},
            {1,
             [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               auto const &cd = ctx.handler().get_calldata();
               uint8_t count = cd.empty() ? 1 : cd[0];
               auto key = make_storage_key("rcq0");
               for (uint8_t i = 0; i < count; i++) {
                 auto res =
                     ctx.handler().retrocausal_queue_consume(key, false);
                 if (!res.has_value())
                   return res.error().to_invoke_error();
               }
               return {};
             }},
        });
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract));
    return std::make_unique<NativeContext>(std::move(contract_map));
  }

  Transaction make_rcq_tx(uint32_t method,
                          std::vector<uint8_t> const &calldata = {})
  {
    return Transaction{
        .invocation = Invocation{
            .invoked_address = kAddrA,
            .entrypoint = entrypoint_from_method_id(method),
            .gas_limit = UINT64_MAX,
            .calldata = Calldata(calldata.begin(), calldata.end())}};
  }

  // Proposal helper for adversarial tests: insert {10} + commit,
  // consume 1 + abort.  Returns both proposal datas.
  void propose_rcq_insert_and_consume(
      RuntimeContext &ctx,
      NondeterministicProposalData &insert_p,
      NondeterministicProposalData &consume_p)
  {
    auto tx_insert = make_rcq_tx(0, {10});
    auto tx_consume = make_rcq_tx(1, {1});

    auto ha = make_new_system_call_handler(tx_insert, ctx);
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);

    auto hb = make_new_system_call_handler(tx_consume, ctx);
    ASSERT_TRUE(hb.root_invoke().is_root_success());
    consume_p = hb.get_proposal_data();
    ASSERT_TRUE(hb.abort_transaction().has_value());
  }

  // Validation helper for adversarial tests: insert TX succeeds,
  // consume TX with tampered proposal data must fail.
  void validate_rcq_tampered_consume_fails(
      RuntimeContext &ctx,
      NondeterministicProposalData const &insert_p,
      NondeterministicProposalData tampered_consume_p)
  {
    auto tx_insert = make_rcq_tx(0, {10});
    auto tx_consume = make_rcq_tx(1, {1});

    auto ha = make_new_system_call_handler_validation(tx_insert, ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);

    auto hb = make_new_system_call_handler_validation(
        tx_consume, ctx, std::move(tampered_consume_p));
    EXPECT_FALSE(hb.root_invoke().is_root_success())
        << "Tampered proposal data should cause validation failure";
    ASSERT_TRUE(hb.abort_transaction().has_value());
  }
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
// RetrocausalQueue operations.  The key code paths are:
//   - concurrent-entry observation replay (item_view.cc)
//   - consumed_but_not_inserted reconciliation (refcounted_delta_variant.cc)
//   - consumed_set check in block finalization (retrocausal_queue_lock.cc)

// Two TXs in same block: TX_A inserts {10}, TX_B consumes the
// concurrent entry.  This produces 1 nondeterministic bit (entry
// exists) and 1 queue entry observation ({10}).  Validates that
// the full propose→validate pipeline preserves these correctly.
TEST_P(ProposeValidateRoundtripTest, RCQConcurrentEntryRoundtrip)
{
  auto ctx = make_rcq_context();
  auto tx_insert = make_rcq_tx(0, {10});
  auto tx_consume = make_rcq_tx(1, {1});

  // === Proposal: TX_A inserts, commits, then TX_B consumes concurrent ===
  NondeterministicProposalData insert_p, consume_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);

    auto hb = make_new_system_call_handler(tx_consume, *ctx);
    ASSERT_TRUE(hb.root_invoke().is_root_success());
    consume_p = hb.get_proposal_data();
    EXPECT_EQ(consume_p.nondeterministic_bit_count, 1U);
    EXPECT_EQ(consume_p.observed_queue_entry_sequence.size(), 1U);
    ASSERT_EQ(hb.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  DbTip proposal_tip;
  if (GetParam() == TestDB::kHogDB) {
    advance_block();
    proposal_tip = get_db_tip();
    discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
  }
  else {
    discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
  }

  // === Validation: replay with captured proposal data ===
  {
    auto ha = make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);

    auto hb =
        make_new_system_call_handler_validation(tx_consume, *ctx, consume_p);
    ASSERT_TRUE(hb.root_invoke().is_root_success());
    ASSERT_EQ(hb.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  if (GetParam() == TestDB::kHogDB) {
    advance_block();
    EXPECT_EQ(proposal_tip.db_hash, get_db_tip().db_hash)
        << "Proposal and validation trie root hashes must match";
  }
}

// Insert 1 entry in block 1.  Block 2: consume past entry (deterministic),
// then try concurrent consume (no entries → bit=false).  Validates that
// the bit=false replay path works correctly.
TEST_P(ProposeValidateRoundtripTest, RCQConsumeEmptyRoundtrip)
{
  auto ctx = make_rcq_context();

  // Block 1: insert 1 entry
  {
    auto tx = make_rcq_tx(0, {1});
    auto handler = make_new_system_call_handler(tx, *ctx);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  }
  advance_block();

  // Block 2: consume 2 (1 past entry + 1 concurrent miss)
  auto tx_consume = make_rcq_tx(1, {2});

  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx_consume, *ctx);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    proposal_data = handler.get_proposal_data();
    EXPECT_EQ(proposal_data.nondeterministic_bit_count, 1U);
    EXPECT_EQ(proposal_data.observed_queue_entry_sequence.size(), 0U);
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto handler =
        make_new_system_call_handler_validation(tx_consume, *ctx, proposal_data);
    ASSERT_TRUE(handler.root_invoke().is_root_success())
        << "Validation with bit=false (empty concurrent) should succeed";
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  }
}

// Single TX inserts {10, 20} then consumes 3 times.  First 2 consumes
// return self-entries (deterministic), third tries concurrent (miss →
// bit=false).  Validates that self-entry + concurrent-miss replay works.
TEST_P(ProposeValidateRoundtripTest, RCQInsertConsumeSameTxRoundtrip)
{
  std::vector<uint8_t> consumed_values;

  // Custom contract: single method that inserts then consumes.
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [&consumed_values](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto key = make_storage_key("rcq0");
             consumed_values.clear();
             for (uint8_t val : {10, 20}) {
               auto r = ctx.handler().retrocausal_queue_insert(
                   key, std::as_bytes(std::span(&val, 1)));
               if (!r.has_value())
                 return r.error().to_invoke_error();
             }
             for (int i = 0; i < 3; i++) {
               auto r = ctx.handler().retrocausal_queue_consume(key, false);
               if (!r.has_value())
                 return r.error().to_invoke_error();
               if (r.value().has_value() && r.value().value().size() == 1)
                 consumed_values.push_back(r.value().value()[0]);
             }
             return {};
           }},
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto ctx = std::make_unique<NativeContext>(std::move(contract_map));

  auto tx = make_rcq_tx(0);

  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx, *ctx);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    proposal_data = handler.get_proposal_data();
    EXPECT_EQ(proposal_data.nondeterministic_bit_count, 1U);
    EXPECT_EQ(consumed_values, (std::vector<uint8_t>{10, 20}));
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto handler =
        make_new_system_call_handler_validation(tx, *ctx, proposal_data);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    EXPECT_EQ(consumed_values, (std::vector<uint8_t>{10, 20}));
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  }
}

// ===================================================================
// Adversarial / Negative RCQ Validation Tests
//
// All four tests use the same scenario: TX_A inserts {10}, TX_B
// consumes the concurrent entry.  Each test tampers with B's proposal
// data in a different way and verifies validation rejects it.
// ===================================================================

// Tamper: add an extra queue entry observation.
// check_all_consumed() detects the surplus → validation fails.
TEST_P(ProposeValidateRoundtripTest, RCQExtraObservationsCauseFailure)
{
  auto ctx = make_rcq_context();
  NondeterministicProposalData insert_p, consume_p;
  propose_rcq_insert_and_consume(*ctx, insert_p, consume_p);

  auto tampered = consume_p;
  QueueEntry extra;
  extra.push_back(99);
  tampered.observed_queue_entry_sequence.push_back(extra);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
  validate_rcq_tampered_consume_fails(*ctx, insert_p, std::move(tampered));
}

// Tamper: remove the queue entry observation (keep bit=true).
// consume_queue_entry_observation returns nullopt → validation fails.
TEST_P(ProposeValidateRoundtripTest, RCQMissingObservationCausesFailure)
{
  auto ctx = make_rcq_context();
  NondeterministicProposalData insert_p, consume_p;
  propose_rcq_insert_and_consume(*ctx, insert_p, consume_p);
  ASSERT_EQ(consume_p.observed_queue_entry_sequence.size(), 1U);

  auto tampered = consume_p;
  tampered.observed_queue_entry_sequence.clear();

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
  validate_rcq_tampered_consume_fails(*ctx, insert_p, std::move(tampered));
}

// Tamper: flip existence bit true→false.
// Validator skips observation consumption → unconsumed observation →
// check_all_consumed() fails.
TEST_P(ProposeValidateRoundtripTest, RCQExistenceBitFlippedToFalse)
{
  auto ctx = make_rcq_context();
  NondeterministicProposalData insert_p, consume_p;
  propose_rcq_insert_and_consume(*ctx, insert_p, consume_p);
  ASSERT_EQ(consume_p.nondeterministic_bit_count, 1U);

  auto tampered = consume_p;
  tampered.nondeterministic_bits[0] ^= 1;

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
  validate_rcq_tampered_consume_fails(*ctx, insert_p, std::move(tampered));
}

// Tamper: flip existence bit false→true.
// Validator tries to consume an observation that doesn't exist → fails.
TEST_P(ProposeValidateRoundtripTest, RCQExistenceBitFlippedToTrue)
{
  auto ctx = make_rcq_context();

  // Need bit=false scenario: insert in block 1, consume in block 2
  {
    auto tx = make_rcq_tx(0, {10});
    auto handler = make_new_system_call_handler(tx, *ctx);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  }
  advance_block();

  // Consume 2: 1 past entry (deterministic) + 1 concurrent miss (bit=false)
  auto tx_consume = make_rcq_tx(1, {2});

  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx_consume, *ctx);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    proposal_data = handler.get_proposal_data();
    ASSERT_EQ(proposal_data.nondeterministic_bit_count, 1U);
    ASSERT_EQ(proposal_data.observed_queue_entry_sequence.size(), 0U);
    ASSERT_TRUE(handler.abort_transaction().has_value());
  }

  auto tampered = proposal_data;
  tampered.nondeterministic_bits[0] ^= 1; // false → true

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto handler =
        make_new_system_call_handler_validation(tx_consume, *ctx,
                                                std::move(tampered));
    EXPECT_FALSE(handler.root_invoke().is_root_success())
        << "Flipped bit (false→true) with no observation should fail";
    ASSERT_TRUE(handler.abort_transaction().has_value());
  }
}

// ===================================================================
// Impossible Commit Set Tests
//
// A malicious proposer fabricates internally-consistent proposal data
// describing an impossible execution.  Per-TX commits succeed (they
// track deltas only), but block finalization detects the inconsistency
// in write_committed_change (retrocausal_queue_lock.cc).
//
// Requires kHogDB — the check is in the hogdb layer.
// ===================================================================

// Proposer claims TX_B consumed {99} but TX_A only inserted {10}.
// Block finalization: consumed_set={99} vs added_entries={10} → error.
TEST_P(ProposeValidateRoundtripTest, RCQFabricatedObservationValue)
{
  if (GetParam() != TestDB::kHogDB)
    GTEST_SKIP() << "write_committed_change check requires kHogDB";

  auto ctx = make_rcq_context();
  auto tx_insert = make_rcq_tx(0, {10});
  auto tx_consume = make_rcq_tx(1, {1});

  NondeterministicProposalData insert_p, consume_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);

    auto hb = make_new_system_call_handler(tx_consume, *ctx);
    ASSERT_TRUE(hb.root_invoke().is_root_success());
    consume_p = hb.get_proposal_data();
    ASSERT_EQ(consume_p.observed_queue_entry_sequence.size(), 1U);
    ASSERT_EQ(hb.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  // Tamper: change observation {10} → {99}
  auto tampered = consume_p;
  tampered.observed_queue_entry_sequence[0] = {99};

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto ha = make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);

    auto hb = make_new_system_call_handler_validation(
        tx_consume, *ctx, std::move(tampered));
    ASSERT_TRUE(hb.root_invoke().is_root_success());
    ASSERT_EQ(hb.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  EXPECT_THROW(advance_block(), std::runtime_error)
      << "Fabricated observation value should fail finalization";
}

// Proposer claims TX_B consumed {10} from TX_A, but TX_A is absent
// from validation (aborted).  No insert → consumed_set non-empty → error.
TEST_P(ProposeValidateRoundtripTest, RCQAbortedInserterEntryConsumed)
{
  if (GetParam() != TestDB::kHogDB)
    GTEST_SKIP() << "write_committed_change check requires kHogDB";

  auto ctx = make_rcq_context();
  auto tx_insert = make_rcq_tx(0, {10});
  auto tx_consume = make_rcq_tx(1, {1});

  NondeterministicProposalData consume_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);

    auto hb = make_new_system_call_handler(tx_consume, *ctx);
    ASSERT_TRUE(hb.root_invoke().is_root_success());
    consume_p = hb.get_proposal_data();
    ASSERT_EQ(hb.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Only TX_B runs — TX_A is "aborted" (omitted from validation).
  {
    auto hb = make_new_system_call_handler_validation(
        tx_consume, *ctx, std::move(consume_p));
    ASSERT_TRUE(hb.root_invoke().is_root_success());
    ASSERT_EQ(hb.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  EXPECT_THROW(advance_block(), std::runtime_error)
      << "Consumed entry from missing inserter should fail finalization";
}

// Two inserters, one consumer.  Proposer claims consumer saw 2 entries,
// but only 1 inserter is included in validation.  Second consumed entry
// has no matching insert → error.
TEST_P(ProposeValidateRoundtripTest, RCQMoreConsumedThanInserted)
{
  if (GetParam() != TestDB::kHogDB)
    GTEST_SKIP() << "write_committed_change check requires kHogDB";

  auto ctx = make_rcq_context();
  auto tx_insert_a = make_rcq_tx(0, {10});
  auto tx_insert_b = make_rcq_tx(0, {20});
  auto tx_consume = make_rcq_tx(1, {2});

  NondeterministicProposalData insert_a_p, consume_p;
  {
    auto ha = make_new_system_call_handler(tx_insert_a, *ctx);
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    insert_a_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);

    auto hb = make_new_system_call_handler(tx_insert_b, *ctx);
    ASSERT_TRUE(hb.root_invoke().is_root_success());
    ASSERT_EQ(hb.try_commit_transaction(), ExecResult::ExecResultOK);

    auto hc = make_new_system_call_handler(tx_consume, *ctx);
    ASSERT_TRUE(hc.root_invoke().is_root_success());
    consume_p = hc.get_proposal_data();
    ASSERT_EQ(consume_p.observed_queue_entry_sequence.size(), 2U);
    ASSERT_EQ(hc.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Only TX_A and TX_C — TX_B "aborted" (not included).
  // TX_C replays with observations {10},{20} but only {10} is inserted.
  {
    auto ha = make_new_system_call_handler_validation(
        tx_insert_a, *ctx, insert_a_p);
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);

    auto hc = make_new_system_call_handler_validation(
        tx_consume, *ctx, std::move(consume_p));
    ASSERT_TRUE(hc.root_invoke().is_root_success());
    ASSERT_EQ(hc.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  EXPECT_THROW(advance_block(), std::runtime_error)
      << "More consumed than inserted should fail finalization";
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, ProposeValidateRoundtripTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
