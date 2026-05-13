#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogexec_constants.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "invoke_flag.hh"
#include "retrocausal_queue.hh"
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

  // --- try_invoke contract helpers ---

  // Method IDs for the cross-contract try_invoke tests.
  static constexpr Runtime::MethodID kWriteHello = 0;
  static constexpr Runtime::MethodID kWriteWorld = 1;

  // Build a NativeContract from a list of method lambdas.
  // Methods are assigned IDs 0, 1, 2, ... in order.
  static std::unique_ptr<NativeContract>
  make_contract(std::initializer_list<NativeContract::function_t> methods)
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> map;
    Runtime::MethodID id = 0;
    for (const auto &m : methods)
      map.emplace(id++, m);
    return std::make_unique<NativeContract>(std::move(map));
  }

  // B-contract method: write a bytestring to storage.
  static NativeContract::function_t make_write_method(std::string_view key_name,
                                                      std::string_view value)
  {
    return [k = std::string(key_name), v = std::string(value)](
               NativeSyscallContext &ctx) -> InvokeStatus<void> {
      auto key = make_storage_key(k);
      auto [ws, _wg] = syscall_BytestringSet_inner(
          ctx.handler(), key, std::as_bytes(std::span(std::string_view(v))),
          std::to_underlying(StorageDeltaFlags::None));
      if (!ws.has_value() ||
          ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
        return InvokeError{InvokeErrorCode::Deterministic};
      }
      return {};
    };
  }

  // Build a B-contract whose methods each write a key-value pair.
  // Method IDs are assigned sequentially starting from 0.
  static std::unique_ptr<NativeContract> make_write_contract(
      std::initializer_list<std::pair<std::string_view, std::string_view>>
          entries)
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> methods;
    Runtime::MethodID id = 0;
    for (auto [key, value] : entries)
      methods.emplace(id++, make_write_method(key, value));
    return std::make_unique<NativeContract>(std::move(methods));
  }

  // Build a NativeContext from contracts A and B.
  static std::unique_ptr<NativeContext>
  make_ab_context(std::unique_ptr<NativeContract> a,
                  std::unique_ptr<NativeContract> b)
  {
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(a));
    contract_map.emplace(1, std::move(b));
    return std::make_unique<NativeContext>(std::move(contract_map));
  }

  // Build a transaction targeting contract A's only method.
  static Transaction make_invoke_a_tx(Runtime::MethodID method)
  {
    return Transaction{
        .invocation =
            Invocation{.invoked_address = kAddrA,
                       .entrypoint = entrypoint_from_method_id(method),
                       .gas_limit = UINT64_MAX,
                       .calldata = {}}};
  }

  // Propose a TX: invoke, assert success, return proposal data.
  // Commits or aborts depending on `commit`.
  NondeterministicProposalData propose(Transaction const &tx,
                                       RuntimeContext &ctx, bool commit = true)
  {
    auto handler = make_new_system_call_handler(tx, ctx);
    auto res = handler.root_invoke();
    EXPECT_TRUE(res.is_root_success());
    auto data = handler.get_proposal_data();
    if (commit) {
      EXPECT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
    }
    else {
      EXPECT_TRUE(handler.abort_transaction().has_value());
    }
    return data;
  }

  // try_invoke helper for use inside A-contract lambdas.
  static auto try_invoke_b(NativeSyscallContext &ctx, Runtime::MethodID method)
  {
    RuntimeMemoryLength output_return_buffer_len = 0,
                        output_returned_auth_token_len = 0;
    return ctx.syscall<SyscallNo::kInvoke>(
        kAddrB, method, ctx.runtime().get_available_gas(), Calldata{},
        std::to_underlying(InvokeFlag::TryInvoke), std::span<uint32_t>{},
        output_return_buffer_len, output_returned_auth_token_len);
  }

  // Build a NativeContext with a single no-op contract (for structural
  // validation tests that don't need real execution).
  static std::unique_ptr<NativeContext> make_noop_context()
  {
    auto noop = make_contract(
        {[](NativeSyscallContext & /*unused*/) -> InvokeStatus<void> {
          return {};
        }});
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(noop));
    return std::make_unique<NativeContext>(std::move(contract_map));
  }

  // --- RCQ test helpers ---

  // Calldata-driven RCQ contract:
  //   method 0: each calldata byte → one queue entry
  //   method 1: consume calldata[0] entries (default 1)
  static std::unique_ptr<RuntimeContext> make_rcq_context()
  {
    auto contract = make_contract({
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
        },
        [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          auto const &cd = ctx.handler().get_calldata();
          uint8_t count = cd.empty() ? 1 : cd[0];
          auto key = make_storage_key("rcq0");
          for (uint8_t i = 0; i < count; i++) {
            auto res = ctx.handler().retrocausal_queue_consume(key, false);
            if (!res.has_value())
              return res.error().to_invoke_error();
          }
          return {};
        },
    });
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(contract));
    return std::make_unique<NativeContext>(std::move(contract_map));
  }

  static Transaction make_rcq_tx(uint32_t method,
                                 std::vector<uint8_t> const &calldata = {})
  {
    return Transaction{
        .invocation =
            Invocation{.invoked_address = kAddrA,
                       .entrypoint = entrypoint_from_method_id(method),
                       .gas_limit = UINT64_MAX,
                       .calldata = Calldata(calldata.begin(), calldata.end())}};
  }

  static Transaction make_rcq_insert_tx(std::vector<uint8_t> const &values)
  {
    return make_rcq_tx(0, values);
  }

  static Transaction make_rcq_consume_tx(uint8_t count)
  {
    return make_rcq_tx(1, {count});
  }

  // Proposal helper for adversarial tests: insert {10} + commit,
  // consume 1 + abort.  Returns both proposal datas.
  std::pair<NondeterministicProposalData, NondeterministicProposalData>
  propose_rcq_insert_and_consume(RuntimeContext &ctx)
  {
    auto tx_insert = make_rcq_insert_tx({10});
    auto tx_consume = make_rcq_consume_tx(1);

    auto ha = make_new_system_call_handler(tx_insert, ctx);
    EXPECT_TRUE(ha.root_invoke().is_root_success());
    auto insert_p = ha.get_proposal_data();
    EXPECT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);

    auto hb = make_new_system_call_handler(tx_consume, ctx);
    EXPECT_TRUE(hb.root_invoke().is_root_success());
    auto consume_p = hb.get_proposal_data();
    EXPECT_TRUE(hb.abort_transaction().has_value());

    return {insert_p, consume_p};
  }

  // Validation helper for adversarial tests: insert TX succeeds,
  // consume TX with tampered proposal data must fail.
  void validate_rcq_tampered_consume_fails(
      RuntimeContext &ctx, NondeterministicProposalData const &insert_p,
      NondeterministicProposalData tampered_consume_p)
  {
    auto tx_insert = make_rcq_insert_tx({10});
    auto tx_consume = make_rcq_consume_tx(1);

    auto ha = make_new_system_call_handler_validation(tx_insert, ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);

    auto hb =
        make_new_system_call_handler_validation(tx_consume, ctx,
                                                std::move(tampered_consume_p));
    EXPECT_FALSE(hb.root_invoke().is_root_success())
        << "Tampered proposal data should cause validation failure";
    ASSERT_TRUE(hb.abort_transaction().has_value());
  }
};

// Contract A: try_invoke B.write_hello, then try_invoke B.write_world.
// Both succeed, producing 2 nondeterministic bits (both true).
// Nondeterministic bits are only logged when the inner frame succeeds
// (deterministic errors skip lock acquisition entirely).
TEST_P(ProposeValidateRoundtripTest, SuccessfulRoundtrip)
{
  auto contract_a =
      make_contract({[](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        if (auto r = try_invoke_b(ctx, kWriteHello); !r.has_value())
          return InvokeError{InvokeErrorCode::Deterministic};
        if (auto r = try_invoke_b(ctx, kWriteWorld); !r.has_value())
          return InvokeError{InvokeErrorCode::Deterministic};

        // Write something to prove we survived both try_invokes
        auto key = make_storage_key("result");
        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), key,
            std::as_bytes(std::span(std::string_view("ok"))),
            std::to_underlying(StorageDeltaFlags::None));
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone)
          return InvokeError{InvokeErrorCode::Deterministic};
        return {};
      }});

  auto native_context =
      make_ab_context(std::move(contract_a),
                      make_write_contract(
                          {{"b_data0", "hello"}, {"b_data1", "world"}}));
  auto tx = make_invoke_a_tx(0);

  auto proposal_data = propose(tx, *native_context);
  EXPECT_EQ(proposal_data.nondeterministic_bit_count, 2U);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Validation with correct proposal data should succeed.
  {
    auto handler = make_new_system_call_handler_validation(tx, *native_context,
                                                           proposal_data);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  }
}

// Test that validation fails when the proposal data has extra unconsumed
// nondeterministic bits (entropy grinding attack vector).
TEST_P(ProposeValidateRoundtripTest, ExtraBitsCauseValidationFailure)
{
  // A: single try_invoke of B.write_hello → produces 1 nondeterministic bit.
  auto contract_a =
      make_contract({[](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        if (auto r = try_invoke_b(ctx, kWriteHello); !r.has_value())
          return InvokeError{InvokeErrorCode::Deterministic};
        return {};
      }});

  auto native_context =
      make_ab_context(std::move(contract_a),
                      make_write_contract({{"b_data0", "hello"}}));
  auto tx = make_invoke_a_tx(0);

  auto proposal_data = propose(tx, *native_context, /*commit=*/false);
  EXPECT_EQ(proposal_data.nondeterministic_bit_count, 1U);

  // Tamper: add an extra bit (bit_count=2 but execution only consumes 1).
  auto tampered = proposal_data;
  tampered.nondeterministic_bit_count = 2;

  auto handler = make_new_system_call_handler_validation(tx, *native_context,
                                                         std::move(tampered));
  // check_all_consumed() should fail because 2 bits were supplied but
  // only 1 was consumed.
  EXPECT_FALSE(handler.root_invoke().is_root_success())
      << "Validation should fail with extra unconsumed bits";
  ASSERT_TRUE(handler.abort_transaction().has_value());
}

// Test that validation fails when proposal data has a flipped bit.
// Contract A does two try_invokes, but only does the second if the first
// succeeds.  In proposal mode both succeed, producing 2 bits (both true).
// Flipping bit 0 to false causes the validator to skip the second
// try_invoke, consuming only 1 of 2 bits → check_all_consumed() fails.
TEST_P(ProposeValidateRoundtripTest, FlippedBitsCauseValidationFailure)
{
  // A: try_invoke B.write_hello, then conditionally try_invoke B.write_world.
  // Both succeed in proposal → 2 bits.  Flipping bit 0 makes validator
  // skip the second try_invoke → 1 of 2 bits consumed → failure.
  auto contract_a =
      make_contract({[](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        auto r0 = try_invoke_b(ctx, kWriteHello);
        if (!r0.has_value())
          return InvokeError{InvokeErrorCode::Deterministic};

        if (r0.value() == SyscallErrorCode::kSyscallErrorCodeNone) {
          if (auto r1 = try_invoke_b(ctx, kWriteWorld); !r1.has_value())
            return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      }});

  auto native_context =
      make_ab_context(std::move(contract_a),
                      make_write_contract(
                          {{"b_data0", "hello"}, {"b_data1", "world"}}));
  auto tx = make_invoke_a_tx(0);

  auto proposal_data = propose(tx, *native_context, /*commit=*/false);
  ASSERT_EQ(proposal_data.nondeterministic_bit_count, 2U);

  // Tamper: flip bit 0 from true to false.
  // With bit 0 = false, the validator treats the first try_invoke as
  // failed, so contract A skips the second try_invoke. Only 1 of the
  // 2 nondeterministic bits is consumed → check_all_consumed() fails.
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

// Test that validation fails when the proposal is structurally malformed
// (e.g. bit count doesn't match byte count).
TEST_P(ProposeValidateRoundtripTest, MalformedProposalRejectedEarly)
{
  auto native_context = make_noop_context();
  auto tx = make_invoke_a_tx(0);

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
  auto native_context = make_noop_context();
  auto tx = make_invoke_a_tx(0);

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
  auto tx_insert = make_rcq_insert_tx({10});
  auto tx_consume = make_rcq_consume_tx(1);

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
    auto ha =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);

    auto hb =
        make_new_system_call_handler_validation(tx_consume, *ctx, consume_p);
    ASSERT_TRUE(hb.root_invoke().is_root_success());
    ASSERT_EQ(hb.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  if (GetParam() == TestDB::kHogDB) {
    advance_block();
    // Note: this test pairs a consume and an insert in the same block, so
    // block-to-block the db hash shouldn't change regardless of execution order
    // of the two operations.
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
    auto tx = make_rcq_insert_tx({1});
    auto handler = make_new_system_call_handler(tx, *ctx);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  }
  advance_block();

  // Block 2: consume 2 (1 past entry + 1 concurrent miss)
  auto tx_consume = make_rcq_consume_tx(2);

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
    auto handler = make_new_system_call_handler_validation(tx_consume, *ctx,
                                                           proposal_data);
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
  auto contract = make_contract(
      {[&consumed_values](NativeSyscallContext &ctx) -> InvokeStatus<void> {
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
      }});
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract));
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
  auto [insert_p, consume_p] = propose_rcq_insert_and_consume(*ctx);

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
  auto [insert_p, consume_p] = propose_rcq_insert_and_consume(*ctx);
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
  auto [insert_p, consume_p] = propose_rcq_insert_and_consume(*ctx);
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
    auto tx = make_rcq_insert_tx({10});
    auto handler = make_new_system_call_handler(tx, *ctx);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  }
  advance_block();

  // Consume 2: 1 past entry (deterministic) + 1 concurrent miss (bit=false)
  auto tx_consume = make_rcq_consume_tx(2);

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
    auto handler = make_new_system_call_handler_validation(tx_consume, *ctx,
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
    GTEST_SKIP() << "consumed_set validation requires kHogDB";

  auto ctx = make_rcq_context();
  auto tx_insert = make_rcq_insert_tx({10});
  auto tx_consume = make_rcq_consume_tx(1);

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
    auto ha =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);

    auto hb = make_new_system_call_handler_validation(tx_consume, *ctx,
                                                      std::move(tampered));
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
    GTEST_SKIP() << "consumed_set validation requires kHogDB";

  auto ctx = make_rcq_context();
  auto tx_insert = make_rcq_insert_tx({10});
  auto tx_consume = make_rcq_consume_tx(1);

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
    auto hb = make_new_system_call_handler_validation(tx_consume, *ctx,
                                                      std::move(consume_p));
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
    GTEST_SKIP() << "consumed_set validation requires kHogDB";

  auto ctx = make_rcq_context();
  auto tx_insert_a = make_rcq_insert_tx({10});
  auto tx_insert_b = make_rcq_insert_tx({20});
  auto tx_consume = make_rcq_consume_tx(2);

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
    auto ha =
        make_new_system_call_handler_validation(tx_insert_a, *ctx, insert_a_p);
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);

    auto hc = make_new_system_call_handler_validation(tx_consume, *ctx,
                                                      std::move(consume_p));
    ASSERT_TRUE(hc.root_invoke().is_root_success());
    ASSERT_EQ(hc.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  EXPECT_THROW(advance_block(), std::runtime_error)
      << "More consumed than inserted should fail finalization";
}

TEST_P(ProposeValidateRoundtripTest,
       ValidationRejectsOverDepthTryInvokeTrueBits)
{
  auto make_depth_poc_key = [](uint32_t index) {
    StorageKeyContractData key{};
    key[0] = static_cast<uint8_t>('d');
    key[1] = static_cast<uint8_t>('e');
    key[2] = static_cast<uint8_t>('p');
    key[3] = static_cast<uint8_t>('t');
    key[4] = static_cast<uint8_t>('h');
    key[5] = static_cast<uint8_t>(index & 0xff);
    key[6] = static_cast<uint8_t>((index >> 8) & 0xff);
    key[7] = static_cast<uint8_t>((index >> 16) & 0xff);
    key[8] = static_cast<uint8_t>((index >> 24) & 0xff);
    return key;
  };

  auto make_index_calldata = [](uint32_t index) {
    Calldata calldata;
    calldata.push_back(static_cast<uint8_t>(index & 0xff));
    calldata.push_back(static_cast<uint8_t>((index >> 8) & 0xff));
    return calldata;
  };

  auto proposal_bit = [](NondeterministicProposalData const &proposal,
                         uint32_t index) {
    return (proposal.nondeterministic_bits[index / 8] &
            (uint8_t{1} << (index % 8))) != 0;
  };

  auto make_all_true_proposal_data = [] {
    NondeterministicProposalData proposal;
    proposal.nondeterministic_bit_count = kMaxSavepointDepth + 1;
    proposal.nondeterministic_bits.resize(
        (proposal.nondeterministic_bit_count + 7) / 8, 0xff);
    auto valid_bits_in_last_byte =
        proposal.nondeterministic_bit_count % 8;
    if (valid_bits_in_last_byte != 0) {
      proposal.nondeterministic_bits.back() =
          static_cast<uint8_t>((uint8_t{1} << valid_bits_in_last_byte) - 1);
    }
    return proposal;
  };

  auto contract_a =
      make_contract({[make_index_calldata](NativeSyscallContext &ctx)
                         -> InvokeStatus<void> {
        uint32_t successes = 0;
        for (uint32_t i = 0; i < kMaxSavepointDepth + 1; i++) {
          RuntimeMemoryLength output_return_buffer_len = 0,
                              output_returned_auth_token_len = 0;
          auto r = ctx.syscall<SyscallNo::kInvoke>(
              kAddrB, 0, ctx.runtime().get_available_gas(),
              make_index_calldata(i),
              std::to_underlying(InvokeFlag::TryInvoke),
              std::span<uint32_t>{}, output_return_buffer_len,
              output_returned_auth_token_len);
          if (!r.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r.value() == SyscallErrorCode::kSyscallErrorCodeNone) {
            successes++;
          }
        }
        if (successes != kMaxSavepointDepth + 1) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      }});

  auto contract_b =
      make_contract({[make_depth_poc_key](NativeSyscallContext &ctx)
                         -> InvokeStatus<void> {
        auto const &calldata = ctx.handler().get_calldata();
        if (calldata.size() != 2) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        uint32_t index = static_cast<uint32_t>(calldata[0]) |
                         (static_cast<uint32_t>(calldata[1]) << 8);
        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), make_depth_poc_key(index),
            std::as_bytes(std::span(calldata)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      }});

  auto native_context =
      make_ab_context(std::move(contract_a), std::move(contract_b));
  auto tx = make_invoke_a_tx(0);

  // Honest proposal cannot produce kMaxSavepointDepth+1 successes.
  {
    auto proposal_handler = make_new_system_call_handler(tx, *native_context);
    auto proposal_res = proposal_handler.root_invoke();
    ASSERT_FALSE(proposal_res.is_root_success());
    ASSERT_EQ(proposal_res.result_.error().code(),
              InvokeErrorCode::Deterministic);

    auto const &honest_proposal = proposal_handler.get_proposal_data();
    // With the fix, proposal does NOT log a bit for the over-depth try.
    ASSERT_EQ(honest_proposal.nondeterministic_bit_count, kMaxSavepointDepth);
    for (uint32_t i = 0; i < kMaxSavepointDepth; ++i) {
      EXPECT_TRUE(proposal_bit(honest_proposal, i));
    }
    ASSERT_TRUE(proposal_handler.abort_transaction().has_value());
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Validation must reject the all-true malicious proposal.
  auto validation_handler = make_new_system_call_handler_validation(
      tx, *native_context, make_all_true_proposal_data());
  ASSERT_FALSE(validation_handler.root_invoke().is_root_success())
      << "Validation must reject an all-true bitstring past the savepoint cap";

  auto commit_res = validation_handler.try_commit_transaction();
  ASSERT_NE(commit_res, ExecResult::ExecResultOK)
      << "Validation must not commit the malicious over-depth child storage";

  advance_block();

  StorageKey cap_crossing_key(StorageKeyType::ContractData);
  cap_crossing_key.contract_data_key() =
      make_depth_poc_key(kMaxSavepointDepth);
  auto cap_crossing_item = lookup_storage_item_from_db(
      AddressAndKey{.address = kAddrB, .key = std::move(cap_crossing_key)});
  EXPECT_FALSE(cap_crossing_item.has_value())
      << "Over-cap child storage must not be persisted";
}

TEST_P(ProposeValidateRoundtripTest,
       ValidationRejectsStaleTrailingBitAtDepthCap)
{
  // A is tolerant: it does NOT propagate child failures, so a single dangling
  // proposal bit reaches finalize_after_root_frame instead of short-circuiting
  // in A's logic.
  auto contract_a =
      make_contract({[](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        for (uint32_t i = 0; i < kMaxSavepointDepth + 1; i++) {
          RuntimeMemoryLength out_ret = 0, out_auth = 0;
          auto r = ctx.syscall<SyscallNo::kInvoke>(
              kAddrB, 0, ctx.runtime().get_available_gas(), Calldata{},
              std::to_underlying(InvokeFlag::TryInvoke),
              std::span<uint32_t>{}, out_ret, out_auth);
          // Tolerant: propagate system errors but ignore user-level failures
          // (including the deterministic depth-cap failure on the last try).
          if (!r.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        return {};
      }});
  auto contract_b = make_contract(
      {[](NativeSyscallContext & /*ctx*/) -> InvokeStatus<void> { return {}; }});

  auto ctx = make_ab_context(std::move(contract_a), std::move(contract_b));
  auto tx = make_invoke_a_tx(0);

  // Stale-format proposal data: kMaxSavepointDepth `true` bits + one
  // trailing `false` bit (what pre-fix proposal would have produced).
  NondeterministicProposalData stale;
  stale.nondeterministic_bit_count = kMaxSavepointDepth + 1;
  size_t nbytes = (stale.nondeterministic_bit_count + 7) / 8;
  stale.nondeterministic_bits.resize(nbytes, 0);
  for (uint32_t i = 0; i < kMaxSavepointDepth; ++i) {
    stale.nondeterministic_bits[i / 8] |=
        static_cast<uint8_t>(uint8_t{1} << (i % 8));
  }
  // Last bit (index kMaxSavepointDepth) stays 0 -- the stale 'false'.

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  auto handler =
      make_new_system_call_handler_validation(tx, *ctx, std::move(stale));
  auto res = handler.root_invoke();
  ASSERT_FALSE(res.is_root_success())
      << "Validation must reject when the proposal has a trailing bit that "
         "the depth-cap branch refuses to consume";
  EXPECT_EQ(res.result_.error().code(), InvokeErrorCode::ValidationFailure);
  ASSERT_NE(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_P(ProposeValidateRoundtripTest, TryInvokeAtCapMinusOneSucceedsRoundtrip)
{
  auto contract_a =
      make_contract({[](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        for (uint32_t i = 0; i + 1 < kMaxSavepointDepth; i++) {
          RuntimeMemoryLength out_ret = 0, out_auth = 0;
          auto r = ctx.syscall<SyscallNo::kInvoke>(
              kAddrB, 0, ctx.runtime().get_available_gas(), Calldata{},
              std::to_underlying(InvokeFlag::TryInvoke),
              std::span<uint32_t>{}, out_ret, out_auth);
          if (!r.has_value() ||
              r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        return {};
      }});
  auto contract_b = make_contract(
      {[](NativeSyscallContext & /*ctx*/) -> InvokeStatus<void> { return {}; }});

  auto ctx = make_ab_context(std::move(contract_a), std::move(contract_b));
  auto tx = make_invoke_a_tx(0);

  // Proposal succeeds and produces exactly (kMaxSavepointDepth - 1) `true` bits.
  auto handler = make_new_system_call_handler(tx, *ctx);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
  auto proposal_data = handler.get_proposal_data();
  EXPECT_EQ(proposal_data.nondeterministic_bit_count,
            static_cast<uint32_t>(kMaxSavepointDepth - 1));
  ASSERT_TRUE(handler.abort_transaction().has_value());

  // Validation with the captured proposal data succeeds.
  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
  auto validation_handler =
      make_new_system_call_handler_validation(tx, *ctx, proposal_data);
  ASSERT_TRUE(validation_handler.root_invoke().is_root_success());
  ASSERT_EQ(validation_handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

// Regression guard: if a future refactor accidentally lets validation's
// depth-cap branch consume a bit (or skips the depth-cap check entirely),
// a proposal with many more `true` bits than the cap would silently roll
// through. Honest proposal can never produce more than kMaxSavepointDepth
// `true` bits.
TEST_P(ProposeValidateRoundtripTest, ValidationRejectsExcessTrueBitsBeyondCap)
{
  static constexpr uint32_t kAttempts = 2 * kMaxSavepointDepth;
  auto contract_a =
      make_contract({[](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        for (uint32_t i = 0; i < kAttempts; i++) {
          RuntimeMemoryLength out_ret = 0, out_auth = 0;
          auto r = ctx.syscall<SyscallNo::kInvoke>(
              kAddrB, 0, ctx.runtime().get_available_gas(), Calldata{},
              std::to_underlying(InvokeFlag::TryInvoke),
              std::span<uint32_t>{}, out_ret, out_auth);
          // Tolerant: propagate system errors, ignore deterministic child
          // failures (including the depth-cap rejection past the cap).
          if (!r.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        return {};
      }});
  auto contract_b = make_contract(
      {[](NativeSyscallContext & /*ctx*/) -> InvokeStatus<void> { return {}; }});

  auto ctx = make_ab_context(std::move(contract_a), std::move(contract_b));
  auto tx = make_invoke_a_tx(0);

  // Malicious proposal: kAttempts bits, all `true`. Honest proposal could
  // never produce more than kMaxSavepointDepth `true` bits, so any prefix
  // longer than that must be rejected by validation.
  NondeterministicProposalData mal;
  mal.nondeterministic_bit_count = kAttempts;
  size_t nbytes = (mal.nondeterministic_bit_count + 7) / 8;
  mal.nondeterministic_bits.resize(nbytes, 0xff);
  auto trailing = mal.nondeterministic_bit_count % 8;
  if (trailing != 0) {
    mal.nondeterministic_bits.back() =
        static_cast<uint8_t>((uint8_t{1} << trailing) - 1);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  auto handler =
      make_new_system_call_handler_validation(tx, *ctx, std::move(mal));
  auto res = handler.root_invoke();
  ASSERT_FALSE(res.is_root_success())
      << "Validation must reject a proposal with more `true` bits than "
         "kMaxSavepointDepth (each `true` bit corresponds to a +1 depth "
         "increment, and depth is capped).";
  ASSERT_NE(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

// Nested-chain depth propagation: A -> TryInvoke(B); B -> kMaxSavepointDepth
// successful TryInvokes of C; C is a no-op. After B's loop, B's
// total_try_invoke_depth_ has accumulated to kMaxSavepointDepth (one per
// C success, propagated up via commit_successful_child_frame). When B's own
// frame finishes -- i.e., A's TryInvoke of B reaches finish_frame --
// depth_cap_reached() fires on B's frame, so no bit is logged (proposal) or
// consumed (validation) for B itself. A sees the TryInvoke of B as a
// deterministic failure.
//
// This exercises depth accumulation through a parent-child boundary
// (B inherits depth from C), distinct from sequential siblings at one level.
TEST_P(ProposeValidateRoundtripTest, NestedTryInvokeChainHitsCapAtParentFrame)
{
  static constexpr ContractAddress kAddrC = make_contract_address(2);

  // Deploy a third native contract at kAddrC, mirroring SetUp() for A and B.
  auto native_contract_c_xdr = make_native_contract(2);
  write_contract(native_contract_c_xdr);
  write_deployment(kAddrC, native_contract_c_xdr);
  setup_grant_storage_token(kAddrC, 1'000'000'000);

  auto contract_a =
      make_contract({[](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        RuntimeMemoryLength out_ret = 0, out_auth = 0;
        auto r = ctx.syscall<SyscallNo::kInvoke>(
            kAddrB, 0, ctx.runtime().get_available_gas(), Calldata{},
            std::to_underlying(InvokeFlag::TryInvoke),
            std::span<uint32_t>{}, out_ret, out_auth);
        if (!r.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      }});
  auto contract_b =
      make_contract({[](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        for (uint32_t i = 0; i < kMaxSavepointDepth; i++) {
          RuntimeMemoryLength out_ret = 0, out_auth = 0;
          auto r = ctx.syscall<SyscallNo::kInvoke>(
              kAddrC, 0, ctx.runtime().get_available_gas(), Calldata{},
              std::to_underlying(InvokeFlag::TryInvoke),
              std::span<uint32_t>{}, out_ret, out_auth);
          if (!r.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        return {};
      }});
  auto contract_c = make_contract(
      {[](NativeSyscallContext & /*ctx*/) -> InvokeStatus<void> { return {}; }});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract_a));
  contract_map.emplace(1, std::move(contract_b));
  contract_map.emplace(2, std::move(contract_c));
  auto ctx = std::make_unique<NativeContext>(std::move(contract_map));

  auto tx = make_invoke_a_tx(0);

  // Proposal: B's frame hits the cap, B's TryInvoke from A is deterministic
  // failure, A propagates. Proposal logs exactly kMaxSavepointDepth bits
  // (one per C success); none for B's own try_invoke.
  auto proposal_handler = make_new_system_call_handler(tx, *ctx);
  auto proposal_res = proposal_handler.root_invoke();
  ASSERT_FALSE(proposal_res.is_root_success());
  EXPECT_EQ(proposal_res.result_.error().code(),
            InvokeErrorCode::Deterministic);
  auto const &proposal_data = proposal_handler.get_proposal_data();
  EXPECT_EQ(proposal_data.nondeterministic_bit_count,
            static_cast<uint32_t>(kMaxSavepointDepth));
  ASSERT_TRUE(proposal_handler.abort_transaction().has_value());

  // Validation round-trips: A -> B -> kMaxSavepointDepth successful C
  // invocations (each consuming a true bit and bumping depth via
  // note_validation_try_invoke_success); B's frame hits the cap; no bit
  // consumed for B; A's TryInvoke of B fails deterministically; A propagates.
  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
  auto validation_handler =
      make_new_system_call_handler_validation(tx, *ctx, proposal_data);
  auto validation_res = validation_handler.root_invoke();
  ASSERT_FALSE(validation_res.is_root_success())
      << "Validation must reach the same deterministic failure as proposal";
  EXPECT_EQ(validation_res.result_.error().code(),
            InvokeErrorCode::Deterministic);
  ASSERT_TRUE(validation_handler.abort_transaction().has_value());
}

INSTANTIATE_TEST_SUITE_P(TestDBs, ProposeValidateRoundtripTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
