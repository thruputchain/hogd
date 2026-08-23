#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hog_exec_constants.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "precompiles.hh"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// End-to-end test: run a transaction in proposal mode, capture the
// NondeterministicProposalData, then replay in validation mode and verify
// the results match.  Also test that tampered proposal data causes
// validation failure.
// NOLINTNEXTLINE(misc-use-internal-linkage)
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
  template<typename... Fns>
  static std::unique_ptr<NativeContract> make_contract(Fns &&...fns)
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> map;
    Runtime::MethodID id = 0;
    (map.emplace(id++, std::forward<Fns>(fns)), ...);
    return std::make_unique<NativeContract>(std::move(map));
  }

  // B-contract method: write a bytestring to storage.
  static NativeContract::function_t make_write_method(std::string_view key_name,
                                                      std::string_view value)
  {
    return [k = std::string(key_name),
            v = std::string(value)](NativeSyscallContext &ctx) noexcept
               -> x23::Async<InvokeStatus<void>> {
      auto key = make_storage_key(k);
      auto [ws, _wg] = syscall_BytestringSet_inner(
          ctx.handler(), key, std::as_bytes(std::span(std::string_view(v))),
          std::to_underlying(StorageDeltaFlags::None))();
      if (!ws.has_value() ||
          ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
        co_return InvokeError{InvokeErrorCode::Deterministic};
      }
      co_return InvokeStatus<void>{};
    };
  }

  // Build a B-contract whose methods each write a key-value pair.
  // Method IDs are assigned sequentially starting from 0.
  static std::unique_ptr<NativeContract> make_write_contract(
      std::initializer_list<std::pair<std::string_view, std::string_view>>
          entries)
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    Runtime::MethodID id = 0;
    for (auto [key, value] : entries)
      fns.emplace(id++, make_write_method(key, value));
    return std::make_unique<NativeContract>(std::move(fns));
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
  Transaction make_invoke_a_tx(Runtime::MethodID method)
  {
    return Transaction{.invocation =
                           Invocation{.invoked_address = kAddrA,
                                      .entrypoint =
                                          entrypoint_from_method_id(method),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = {}},
                       .uniquifier = make_new_uniquifier()};
  }

  // Propose a TX: invoke, assert success, return proposal data.
  // Commits or aborts depending on `commit`.
  NondeterministicProposalData propose(Transaction const &tx,
                                       RuntimeContext &ctx, bool commit = true)
  {
    auto handler = make_new_system_call_handler(tx, ctx);
    auto res = handler.root_invoke()();
    EXPECT_TRUE(res.is_root_success());
    auto data = handler.get_proposal_data();
    if (commit) {
      EXPECT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
    }
    else {
      EXPECT_TRUE(handler.abort_transaction()().has_value());
    }
    return data;
  }

  // try_invoke helper for use inside A-contract lambdas.
  static auto try_invoke_b(NativeSyscallContext &ctx, Runtime::MethodID method)
  {
    CopyableInvokeInputs inputs{
        .entrypoint_id = method,
        .gas_limit = UINT64_MAX,
        .flags = std::to_underlying(InvokeFlag::TryInvoke),
    };
    return co_await ctx.syscall<SyscallNo::kInvoke>(
        kAddrB, std::span<const std::byte>{}, std::span<uint32_t>{}, inputs,
        CopyableInvokeOutputs{});
  }

  // Same as try_invoke_b, but writes the child frame outcome into `outputs`
  // so callers can distinguish a committed try_invoke (frame_status == Return)
  // from one that ran but rolled back (frame_status != Return). Under the
  // current Invoke ABI both cases return kSyscallErrorCodeNone, so the
  // frame_status channel is the only way to tell them apart.
  static auto try_invoke_b(NativeSyscallContext &ctx, Runtime::MethodID method,
                           CopyableInvokeOutputs &outputs)
  {
    CopyableInvokeInputs inputs{
        .entrypoint_id = method,
        .gas_limit = UINT64_MAX,
        .flags = std::to_underlying(InvokeFlag::TryInvoke),
    };
    return co_await ctx.syscall<SyscallNo::kInvoke>(
        kAddrB, std::span<const std::byte>{}, std::span<uint32_t>{}, inputs,
        outputs);
  }

  // Build a NativeContext with a single no-op contract (for structural
  // validation tests that don't need real execution).
  static std::unique_ptr<NativeContext> make_noop_context()
  {
    auto noop = make_contract([](NativeSyscallContext & /*unused*/) noexcept
                                  -> x23::Async<InvokeStatus<void>> {
      co_return InvokeStatus<void>{};
    });
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
    auto contract = make_contract(
        [](NativeSyscallContext &ctx) noexcept
            -> x23::Async<InvokeStatus<void>> {
          auto const &cd = ctx.handler().get_calldata();
          auto key = make_storage_key("rcq0");
          for (uint8_t val : cd) {
            auto res = ctx.handler().retrocausal_queue_insert(
                key, std::as_bytes(std::span(&val, 1)))();
            if (!res.has_value())
              co_return res.error().to_invoke_error();
          }
          co_return InvokeStatus<void>{};
        },
        [](NativeSyscallContext &ctx) noexcept
            -> x23::Async<InvokeStatus<void>> {
          auto const &cd = ctx.handler().get_calldata();
          uint8_t count = cd.empty() ? 1 : cd[0];
          auto key = make_storage_key("rcq0");
          for (uint8_t i = 0; i < count; i++) {
            auto res = ctx.handler().retrocausal_queue_consume(key, false)();
            if (!res.has_value())
              co_return res.error().to_invoke_error();
          }
          co_return InvokeStatus<void>{};
        });
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(contract));
    return std::make_unique<NativeContext>(std::move(contract_map));
  }

  Transaction make_rcq_tx(uint32_t method,
                          std::vector<uint8_t> const &calldata = {})
  {
    return Transaction{
        .invocation =
            Invocation{.invoked_address = kAddrA,
                       .entrypoint = entrypoint_from_method_id(method),
                       .gas_limit = UINT64_MAX,
                       .calldata = Calldata(calldata.begin(), calldata.end())},
        .uniquifier = make_new_uniquifier()};
  }

  Transaction make_rcq_insert_tx(std::vector<uint8_t> const &values)
  {
    return make_rcq_tx(0, values);
  }

  Transaction make_rcq_consume_tx(uint8_t count)
  {
    return make_rcq_tx(1, {count});
  }

  // Proposal helper for adversarial tests: insert {10} + commit,
  // consume 1 + abort.  Returns both proposal data.
  std::pair<NondeterministicProposalData, NondeterministicProposalData>
  propose_rcq_insert_and_consume(RuntimeContext &ctx)
  {
    auto tx_insert = make_rcq_insert_tx({10});
    auto tx_consume = make_rcq_consume_tx(1);

    auto ha = make_new_system_call_handler(tx_insert, ctx);
    EXPECT_TRUE(ha.root_invoke()().is_root_success());
    auto insert_p = ha.get_proposal_data();
    EXPECT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto hb = make_new_system_call_handler(tx_consume, ctx);
    EXPECT_TRUE(hb.root_invoke()().is_root_success());
    auto consume_p = hb.get_proposal_data();
    EXPECT_TRUE(hb.abort_transaction()().has_value());

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
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto hb =
        make_new_system_call_handler_validation(tx_consume, ctx,
                                                std::move(tampered_consume_p));
    EXPECT_FALSE(hb.root_invoke()().is_root_success())
        << "Tampered proposal data should cause validation failure";
    ASSERT_TRUE(hb.abort_transaction()().has_value());
  }
};

// Contract A: try_invoke B.write_hello, then try_invoke B.write_world.
// Both succeed, producing 2 nondeterministic bits (both true).
// Nondeterministic bits are only logged when the inner frame succeeds
// (deterministic errors skip lock acquisition entirely).
TEST_P(ProposeValidateRoundtripTest, SuccessfulRoundtrip)
{
  auto contract_a = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        if (auto r = try_invoke_b(ctx, kWriteHello); !r.has_value())
          co_return InvokeError{InvokeErrorCode::Deterministic};
        if (auto r = try_invoke_b(ctx, kWriteWorld); !r.has_value())
          co_return InvokeError{InvokeErrorCode::Deterministic};

        // Write something to prove we survived both try_invokes
        auto key = make_storage_key("result");
        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), key,
            std::as_bytes(std::span(std::string_view("ok"))),
            std::to_underlying(StorageDeltaFlags::None))();
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone)
          co_return InvokeError{InvokeErrorCode::Deterministic};
        co_return InvokeStatus<void>{};
      });

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
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
}

// Test that validation fails when the proposal data has extra unconsumed
// nondeterministic bits (entropy grinding attack vector).
TEST_P(ProposeValidateRoundtripTest, ExtraBitsCauseValidationFailure)
{
  // A: single try_invoke of B.write_hello → produces 1 nondeterministic bit.
  auto contract_a = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        if (auto r = try_invoke_b(ctx, kWriteHello); !r.has_value())
          co_return InvokeError{InvokeErrorCode::Deterministic};
        co_return InvokeStatus<void>{};
      });

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
  EXPECT_FALSE(handler.root_invoke()().is_root_success())
      << "Validation should fail with extra unconsumed bits";
  ASSERT_TRUE(handler.abort_transaction()().has_value());
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
  auto contract_a = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        CopyableInvokeOutputs r0_outputs{};
        auto r0 = try_invoke_b(ctx, kWriteHello, r0_outputs);
        if (!r0.has_value())
          co_return InvokeError{InvokeErrorCode::Deterministic};

        // Only do the second try_invoke if the first one actually committed.
        // A child frame that ran but rolled back also returns
        // kSyscallErrorCodeNone, so success is frame_status == Return. In
        // validation, flipping bit 0 forces this first try_invoke to be
        // replayed as a rollback, so this branch is correctly skipped.
        if (r0.value() == SyscallErrorCode::kSyscallErrorCodeNone &&
            r0_outputs.frame_status == InvocationFrameStatusCode::Return) {
          if (auto r1 = try_invoke_b(ctx, kWriteWorld); !r1.has_value())
            co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

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
  auto res = handler.root_invoke()();
  // The root invoke succeeds (contract A returns Ok), but
  // check_final_validation_data detects the unconsumed bit.
  if (res.is_root_success()) {
    auto commit_res = handler.try_commit_transaction()();
    EXPECT_NE(commit_res, ExecResult::ExecResultOK)
        << "Validation with flipped bits should not commit successfully";
  }
  else {
    // Root invoke failed — also acceptable
    ASSERT_TRUE(handler.abort_transaction()().has_value());
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
  auto res = handler.root_invoke()();
  // validate() should reject this in perform_initial_validation
  EXPECT_FALSE(res.is_root_success())
      << "Malformed proposal (bit count/byte count mismatch) should be "
         "rejected";
  ASSERT_TRUE(handler.abort_transaction()().has_value());
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
  auto res = handler.root_invoke()();
  EXPECT_FALSE(res.is_root_success())
      << "Padding bits set in last byte should be rejected (entropy "
         "grinding)";
  ASSERT_TRUE(handler.abort_transaction()().has_value());
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
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto hb = make_new_system_call_handler(tx_consume, *ctx);
    ASSERT_TRUE(hb.root_invoke()().is_root_success());
    consume_p = hb.get_proposal_data();
    EXPECT_EQ(consume_p.nondeterministic_bit_count, 1U);
    EXPECT_EQ(consume_p.observed_queue_entry_sequence.size(), 1U);
    ASSERT_EQ(hb.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Capture the proposal block's resulting trie root, then reset to the
  // pre-proposal state. discard_block closes the block (finalizing its hash)
  // and abandons it WITHOUT committing, so the committed tip is unchanged and
  // validation replays against the same pre-state -- consuming the same replay
  // nonces fresh rather than colliding with a committed proposal block.
  StorageShortHash proposal_hash =
      discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // === Validation: replay with captured proposal data ===
  {
    auto ha =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto hb =
        make_new_system_call_handler_validation(tx_consume, *ctx, consume_p);
    ASSERT_TRUE(hb.root_invoke()().is_root_success());
    ASSERT_EQ(hb.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Validation must reproduce the proposal's resulting state exactly: closing
  // the validation block yields its trie root for comparison. Both blocks were
  // built from the same pre-state with the same transactions and replay nonces,
  // so the roots must match. (Only HogDb computes real trie roots.)
  StorageShortHash validation_hash =
      discard_block(BlockExecutionMode::kBlockExecutionModeProposal);
  if (GetParam() == TestDB::kHogDb) {
    EXPECT_EQ(proposal_hash, validation_hash)
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
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  advance_block();

  // Block 2: consume 2 (1 past entry + 1 concurrent miss)
  auto tx_consume = make_rcq_consume_tx(2);

  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx_consume, *ctx);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    proposal_data = handler.get_proposal_data();
    EXPECT_EQ(proposal_data.nondeterministic_bit_count, 1U);
    EXPECT_EQ(proposal_data.observed_queue_entry_sequence.size(), 0U);
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto handler = make_new_system_call_handler_validation(tx_consume, *ctx,
                                                           proposal_data);
    ASSERT_TRUE(handler.root_invoke()().is_root_success())
        << "Validation with bit=false (empty concurrent) should succeed";
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
}

// Single TX inserts {10, 20} then consumes 3 times.  First 2 consumes
// return self-entries (deterministic), third tries concurrent (miss →
// bit=false).  Validates that self-entry + concurrent-miss replay works.
TEST_P(ProposeValidateRoundtripTest, RCQInsertConsumeSameTxRoundtrip)
{
  std::vector<uint8_t> consumed_values;

  // Custom contract: single method that inserts then consumes.
  auto contract =
      make_contract([&consumed_values](NativeSyscallContext &ctx) noexcept
                        -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        consumed_values.clear();
        for (uint8_t val : {10, 20}) {
          auto r = ctx.handler().retrocausal_queue_insert(
              key, std::as_bytes(std::span(&val, 1)))();
          if (!r.has_value())
            co_return r.error().to_invoke_error();
        }
        for (int i = 0; i < 3; i++) {
          auto r = ctx.handler().retrocausal_queue_consume(key, false)();
          if (!r.has_value())
            co_return r.error().to_invoke_error();
          if (r.value().has_value() && r.value().value().size() == 1)
            consumed_values.push_back(r.value().value()[0]);
        }
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract));
  auto ctx = std::make_unique<NativeContext>(std::move(contract_map));

  auto tx = make_rcq_tx(0);

  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx, *ctx);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    proposal_data = handler.get_proposal_data();
    EXPECT_EQ(proposal_data.nondeterministic_bit_count, 1U);
    EXPECT_EQ(consumed_values, (std::vector<uint8_t>{10, 20}));
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto handler =
        make_new_system_call_handler_validation(tx, *ctx, proposal_data);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    EXPECT_EQ(consumed_values, (std::vector<uint8_t>{10, 20}));
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
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
// consume_queue_entry_observation returns nullptr → validation fails.
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
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  advance_block();

  // Consume 2: 1 past entry (deterministic) + 1 concurrent miss (bit=false)
  auto tx_consume = make_rcq_consume_tx(2);

  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx_consume, *ctx);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    proposal_data = handler.get_proposal_data();
    ASSERT_EQ(proposal_data.nondeterministic_bit_count, 1U);
    ASSERT_EQ(proposal_data.observed_queue_entry_sequence.size(), 0U);
    ASSERT_TRUE(handler.abort_transaction()().has_value());
  }

  auto tampered = proposal_data;
  tampered.nondeterministic_bits[0] ^= 1; // false → true

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto handler = make_new_system_call_handler_validation(tx_consume, *ctx,
                                                           std::move(tampered));
    EXPECT_FALSE(handler.root_invoke()().is_root_success())
        << "Flipped bit (false→true) with no observation should fail";
    ASSERT_TRUE(handler.abort_transaction()().has_value());
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
// Requires kHogDb — the check is in the hog_db layer.
// ===================================================================

// Proposer claims TX_B consumed {99} but TX_A only inserted {10}.
// Block finalization: consumed_set={99} vs added_entries={10} → error.
TEST_P(ProposeValidateRoundtripTest, RCQFabricatedObservationValue)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "consumed_set validation requires kHogDb";

  auto ctx = make_rcq_context();
  auto tx_insert = make_rcq_insert_tx({10});
  auto tx_consume = make_rcq_consume_tx(1);

  NondeterministicProposalData insert_p, consume_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto hb = make_new_system_call_handler(tx_consume, *ctx);
    ASSERT_TRUE(hb.root_invoke()().is_root_success());
    consume_p = hb.get_proposal_data();
    ASSERT_EQ(consume_p.observed_queue_entry_sequence.size(), 1U);
    ASSERT_EQ(hb.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Tamper: change observation {10} → {99}
  auto tampered = consume_p;
  tampered.observed_queue_entry_sequence[0] = {99};

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto ha =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto hb = make_new_system_call_handler_validation(tx_consume, *ctx,
                                                      std::move(tampered));
    ASSERT_TRUE(hb.root_invoke()().is_root_success());
    ASSERT_EQ(hb.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  EXPECT_FALSE(try_advance_block().has_value())
      << "Fabricated observation value should fail finalization";
}

// Proposer claims TX_B consumed {10} from TX_A, but TX_A is absent
// from validation (aborted).  No insert → consumed_set non-empty → error.
TEST_P(ProposeValidateRoundtripTest, RCQAbortedInserterEntryConsumed)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "consumed_set validation requires kHogDb";

  auto ctx = make_rcq_context();
  auto tx_insert = make_rcq_insert_tx({10});
  auto tx_consume = make_rcq_consume_tx(1);

  NondeterministicProposalData consume_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto hb = make_new_system_call_handler(tx_consume, *ctx);
    ASSERT_TRUE(hb.root_invoke()().is_root_success());
    consume_p = hb.get_proposal_data();
    ASSERT_EQ(hb.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Only TX_B runs — TX_A is "aborted" (omitted from validation).
  {
    auto hb = make_new_system_call_handler_validation(tx_consume, *ctx,
                                                      std::move(consume_p));
    ASSERT_TRUE(hb.root_invoke()().is_root_success());
    ASSERT_EQ(hb.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  EXPECT_FALSE(try_advance_block().has_value())
      << "Consumed entry from missing inserter should fail finalization";
}

// Two inserters, one consumer.  Proposer claims consumer saw 2 entries,
// but only 1 inserter is included in validation.  Second consumed entry
// has no matching insert → error.
TEST_P(ProposeValidateRoundtripTest, RCQMoreConsumedThanInserted)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "consumed_set validation requires kHogDb";

  auto ctx = make_rcq_context();
  auto tx_insert_a = make_rcq_insert_tx({10});
  auto tx_insert_b = make_rcq_insert_tx({20});
  auto tx_consume = make_rcq_consume_tx(2);

  NondeterministicProposalData insert_a_p, consume_p;
  {
    auto ha = make_new_system_call_handler(tx_insert_a, *ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_a_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto hb = make_new_system_call_handler(tx_insert_b, *ctx);
    ASSERT_TRUE(hb.root_invoke()().is_root_success());
    ASSERT_EQ(hb.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto hc = make_new_system_call_handler(tx_consume, *ctx);
    ASSERT_TRUE(hc.root_invoke()().is_root_success());
    consume_p = hc.get_proposal_data();
    ASSERT_EQ(consume_p.observed_queue_entry_sequence.size(), 2U);
    ASSERT_EQ(hc.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Only TX_A and TX_C — TX_B "aborted" (not included).
  // TX_C replays with observations {10},{20} but only {10} is inserted.
  {
    auto ha =
        make_new_system_call_handler_validation(tx_insert_a, *ctx, insert_a_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto hc = make_new_system_call_handler_validation(tx_consume, *ctx,
                                                      std::move(consume_p));
    ASSERT_TRUE(hc.root_invoke()().is_root_success());
    ASSERT_EQ(hc.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  EXPECT_FALSE(try_advance_block().has_value())
      << "More consumed than inserted should fail finalization";
}

// Root manual revert must still submit observation deltas to HogDb.
// A forged observation on an empty queue that triggers manual revert
// must be rejected at block close.
TEST_P(ProposeValidateRoundtripTest,
       RootManualRevertSkipsForgedRCQObservationValidation)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  static constexpr uint8_t kForgedEntry = 0xE7;

  auto revert_on_forged_observation = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto observed = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!observed.has_value())
          co_return observed.error().to_invoke_error();

        if (observed.value().has_value() &&
            observed.value().value().size() == 1 &&
            observed.value().value()[0] == kForgedEntry) {
          ctx.handler().set_return_buffer(ReturnBuffer{}, true);
          co_return InvokeStatus<void>{};
        }

        auto marker = std::string_view("honest");
        auto [write_status, _gas] = syscall_BytestringSet_inner(
            ctx.handler(), make_storage_key("marker"),
            std::as_bytes(std::span(marker)),
            std::to_underlying(StorageDeltaFlags::None))();
        if (!write_status.has_value() ||
            write_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(0, std::move(revert_on_forged_observation));
  auto ctx = std::make_unique<NativeContext>(std::move(cmap));
  auto tx = make_invoke_a_tx(0);

  {
    auto proposal_handler = make_new_system_call_handler(tx, *ctx);
    ASSERT_TRUE(proposal_handler.root_invoke()().is_root_success());
    auto honest_proposal = proposal_handler.get_proposal_data();
    ASSERT_EQ(honest_proposal.nondeterministic_bit_count, 1U);
    ASSERT_EQ(honest_proposal.observed_queue_entry_sequence.size(), 0U);
    ASSERT_EQ(honest_proposal.nondeterministic_bits[0] & uint8_t{1}, 0U)
        << "Honest proposal on an empty queue must observe no entry";
    ASSERT_EQ(proposal_handler.try_commit_transaction()(),
              ExecResult::ExecResultOK);
  }

  NondeterministicProposalData forged;
  forged.nondeterministic_bit_count = 1;
  forged.nondeterministic_bits = {uint8_t{1}};
  forged.observed_queue_entry_sequence = {QueueEntry{kForgedEntry}};

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  auto validation_handler =
      make_new_system_call_handler_validation(tx, *ctx, std::move(forged));
  ASSERT_TRUE(validation_handler.root_invoke()().is_root_success())
      << "The forged observation is consumed and makes the contract choose "
         "manual root-frame revert";
  ASSERT_EQ(validation_handler.try_commit_transaction()(),
            ExecResult::ExecResultTxRootFrameReverted)
      << "Manual root revert commits through the revert branch";
  EXPECT_FALSE(
      try_advance_block(BlockExecutionMode::kBlockExecutionModeValidation)
          .has_value())
      << "Block close must reject the forged observation with no matching "
         "insert";
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
    auto valid_bits_in_last_byte = proposal.nondeterministic_bit_count % 8;
    if (valid_bits_in_last_byte != 0) {
      proposal.nondeterministic_bits.back() =
          static_cast<uint8_t>((uint8_t{1} << valid_bits_in_last_byte) - 1);
    }
    return proposal;
  };

  auto contract_a =
      make_contract([make_index_calldata](NativeSyscallContext &ctx) noexcept
                        -> x23::Async<InvokeStatus<void>> {
        uint32_t successes = 0;
        for (uint32_t i = 0; i < kMaxSavepointDepth + 1; i++) {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          CopyableInvokeOutputs outputs{};
          auto r =
              co_await ctx.syscall<SyscallNo::kInvoke>(kAddrB,
                                                       make_index_calldata(i),
                                                       std::span<uint32_t>{},
                                                       inputs, outputs);
          if (!r.has_value()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          // Count only try_invokes that actually committed. A child frame
          // that ran but rolled back (e.g. the depth-cap rejection on the
          // final attempt) also returns kSyscallErrorCodeNone, so commit is
          // identified by frame_status == Return.
          if (r.value() == SyscallErrorCode::kSyscallErrorCodeNone &&
              outputs.frame_status == InvocationFrameStatusCode::Return) {
            successes++;
          }
        }
        if (successes != kMaxSavepointDepth + 1) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  auto contract_b =
      make_contract([make_depth_poc_key](NativeSyscallContext &ctx) noexcept
                        -> x23::Async<InvokeStatus<void>> {
        auto const &calldata = ctx.handler().get_calldata();
        if (calldata.size() != 2) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        uint32_t index = static_cast<uint32_t>(calldata[0]) |
                         (static_cast<uint32_t>(calldata[1]) << 8);
        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), make_depth_poc_key(index),
            std::as_bytes(std::span(calldata)),
            std::to_underlying(StorageDeltaFlags::None))();
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  auto native_context =
      make_ab_context(std::move(contract_a), std::move(contract_b));
  auto tx = make_invoke_a_tx(0);

  // Honest proposal cannot produce kMaxSavepointDepth+1 successes.
  // The child TryInvoke cap is kMaxSavepointDepth - 1 (one slot reserved
  // for root commit).
  static constexpr uint32_t kChildCap = kMaxSavepointDepth - 1;
  {
    auto proposal_handler = make_new_system_call_handler(tx, *native_context);
    auto proposal_res = proposal_handler.root_invoke()();
    ASSERT_FALSE(proposal_res.is_root_success());
    ASSERT_EQ(proposal_res.result_.error().code(),
              InvokeErrorCode::Deterministic);

    auto const &honest_proposal = proposal_handler.get_proposal_data();
    ASSERT_EQ(honest_proposal.nondeterministic_bit_count, kChildCap);
    for (uint32_t i = 0; i < kChildCap; ++i) {
      EXPECT_TRUE(proposal_bit(honest_proposal, i));
    }
    ASSERT_TRUE(proposal_handler.abort_transaction()().has_value());
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Validation must reject the all-true malicious proposal.
  auto validation_handler =
      make_new_system_call_handler_validation(tx, *native_context,
                                              make_all_true_proposal_data());
  ASSERT_FALSE(validation_handler.root_invoke()().is_root_success())
      << "Validation must reject an all-true bitstring past the savepoint cap";

  auto commit_res = validation_handler.try_commit_transaction()();
  ASSERT_NE(commit_res, ExecResult::ExecResultOK)
      << "Validation must not commit the malicious over-depth child storage";

  advance_block();

  StorageKey cap_crossing_key(StorageKeyType::ContractData);
  cap_crossing_key.contract_data_key() = make_depth_poc_key(kMaxSavepointDepth);
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
  auto contract_a = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        for (uint32_t i = 0; i < kMaxSavepointDepth + 1; i++) {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          auto r = co_await ctx.syscall<SyscallNo::kInvoke>(
              kAddrB, std::span<const std::byte>{}, std::span<uint32_t>{},
              inputs, CopyableInvokeOutputs{});
          // Tolerant: propagate system errors but ignore user-level failures
          // (including the deterministic depth-cap failure on the last try).
          if (!r.has_value()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        co_return InvokeStatus<void>{};
      });
  auto contract_b = make_contract([](NativeSyscallContext & /*ctx*/) noexcept
                                      -> x23::Async<InvokeStatus<void>> {
    co_return InvokeStatus<void>{};
  });

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
  auto res = handler.root_invoke()();
  ASSERT_FALSE(res.is_root_success())
      << "Validation must reject when the proposal has a trailing bit that "
         "the depth-cap branch refuses to consume";
  EXPECT_EQ(res.result_.error().code(), InvokeErrorCode::ValidationFailure);
  ASSERT_NE(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_P(ProposeValidateRoundtripTest, TryInvokeAtCapMinusOneSucceedsRoundtrip)
{
  auto contract_a = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        for (uint32_t i = 0; i + 1 < kMaxSavepointDepth; i++) {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          auto r = co_await ctx.syscall<SyscallNo::kInvoke>(
              kAddrB, std::span<const std::byte>{}, std::span<uint32_t>{},
              inputs, CopyableInvokeOutputs{});
          if (!r.has_value() ||
              r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        co_return InvokeStatus<void>{};
      });
  auto contract_b = make_contract([](NativeSyscallContext & /*ctx*/) noexcept
                                      -> x23::Async<InvokeStatus<void>> {
    co_return InvokeStatus<void>{};
  });

  auto ctx = make_ab_context(std::move(contract_a), std::move(contract_b));
  auto tx = make_invoke_a_tx(0);

  // Proposal succeeds and produces exactly (kMaxSavepointDepth - 1) `true`
  // bits.
  auto handler = make_new_system_call_handler(tx, *ctx);
  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  auto proposal_data = handler.get_proposal_data();
  EXPECT_EQ(proposal_data.nondeterministic_bit_count,
            static_cast<uint32_t>(kMaxSavepointDepth - 1));
  ASSERT_TRUE(handler.abort_transaction()().has_value());

  // Validation with the captured proposal data succeeds.
  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
  auto validation_handler =
      make_new_system_call_handler_validation(tx, *ctx, proposal_data);
  ASSERT_TRUE(validation_handler.root_invoke()().is_root_success());
  ASSERT_EQ(validation_handler.try_commit_transaction()(),
            ExecResult::ExecResultOK);
}

// Regression guard: if a future refactor accidentally lets validation's
// depth-cap branch consume a bit (or skips the depth-cap check entirely),
// a proposal with many more `true` bits than the cap would silently roll
// through. Honest proposal can never produce more than kMaxSavepointDepth
// `true` bits.
TEST_P(ProposeValidateRoundtripTest, ValidationRejectsExcessTrueBitsBeyondCap)
{
  static constexpr uint32_t kAttempts = 2 * kMaxSavepointDepth;
  auto contract_a = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        for (uint32_t i = 0; i < kAttempts; i++) {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          auto r = co_await ctx.syscall<SyscallNo::kInvoke>(
              kAddrB, std::span<const std::byte>{}, std::span<uint32_t>{},
              inputs, CopyableInvokeOutputs{});
          // Tolerant: propagate system errors, ignore deterministic child
          // failures (including the depth-cap rejection past the cap).
          if (!r.has_value()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        co_return InvokeStatus<void>{};
      });
  auto contract_b = make_contract([](NativeSyscallContext & /*ctx*/) noexcept
                                      -> x23::Async<InvokeStatus<void>> {
    co_return InvokeStatus<void>{};
  });

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
  auto res = handler.root_invoke()();
  ASSERT_FALSE(res.is_root_success())
      << "Validation must reject a proposal with more `true` bits than "
         "kMaxSavepointDepth (each `true` bit corresponds to a +1 depth "
         "increment, and depth is capped).";
  ASSERT_NE(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// Nested-chain depth propagation: A -> TryInvoke(B); B ->
// (kMaxSavepointDepth - 1) successful TryInvokes of C; C is a no-op.
// After B's loop, B's total_try_invoke_depth_ has accumulated to
// kMaxSavepointDepth - 1. When B's own frame finishes -- i.e., A's
// TryInvoke of B reaches finish_frame -- depth_cap_reached() fires on B's
// frame, so no bit is logged (proposal) or consumed (validation) for B
// itself. A sees the TryInvoke of B as a deterministic failure.
//
// This exercises depth accumulation through a parent-child boundary
// (B inherits depth from C), distinct from sequential siblings at one level.
TEST_P(ProposeValidateRoundtripTest, NestedTryInvokeChainHitsCapAtParentFrame)
{
  static constexpr ContractAddress kAddrC = make_contract_address(2);
  static constexpr uint32_t kChildCap = kMaxSavepointDepth - 1;

  // Deploy a third native contract at kAddrC, mirroring SetUp() for A and B.
  auto native_contract_c_xdr = make_native_contract(2);
  write_contract(native_contract_c_xdr);
  write_deployment(kAddrC, native_contract_c_xdr);
  setup_grant_storage_token(kAddrC, 1'000'000'000);

  auto contract_a = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = std::to_underlying(InvokeFlag::TryInvoke),
        };
        CopyableInvokeOutputs outputs{};
        auto r = co_await ctx.syscall<SyscallNo::kInvoke>(
            kAddrB, std::span<const std::byte>{}, std::span<uint32_t>{}, inputs,
            outputs);
        if (!r.has_value()) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        // B's frame hits the depth cap and rolls back. The child frame still
        // ran, so the Invoke syscall returns kSyscallErrorCodeNone; the
        // rollback shows up as frame_status != Return. Propagate it as a
        // deterministic failure.
        if (r.value() == SyscallErrorCode::kSyscallErrorCodeNone &&
            outputs.frame_status != InvocationFrameStatusCode::Return) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto contract_b = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        for (uint32_t i = 0; i < kChildCap; i++) {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          CopyableInvokeOutputs outputs{};
          auto r = co_await ctx.syscall<SyscallNo::kInvoke>(
              kAddrC, std::span<const std::byte>{}, std::span<uint32_t>{},
              inputs, outputs);
          if (!r.has_value()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          // These C invocations are expected to commit. A child frame that
          // ran but rolled back also returns kSyscallErrorCodeNone, so a
          // non-commit is detected via frame_status != Return.
          if (r.value() == SyscallErrorCode::kSyscallErrorCodeNone &&
              outputs.frame_status != InvocationFrameStatusCode::Return) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        co_return InvokeStatus<void>{};
      });
  auto contract_c = make_contract([](NativeSyscallContext & /*ctx*/) noexcept
                                      -> x23::Async<InvokeStatus<void>> {
    co_return InvokeStatus<void>{};
  });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract_a));
  contract_map.emplace(1, std::move(contract_b));
  contract_map.emplace(2, std::move(contract_c));
  auto ctx = std::make_unique<NativeContext>(std::move(contract_map));

  auto tx = make_invoke_a_tx(0);

  // Proposal: B's frame hits the cap, B's TryInvoke from A is deterministic
  // failure, A propagates. Proposal logs exactly kChildCap bits
  // (one per C success); none for B's own try_invoke.
  auto proposal_handler = make_new_system_call_handler(tx, *ctx);
  auto proposal_res = proposal_handler.root_invoke()();
  ASSERT_FALSE(proposal_res.is_root_success());
  EXPECT_EQ(proposal_res.result_.error().code(),
            InvokeErrorCode::Deterministic);
  auto const &proposal_data = proposal_handler.get_proposal_data();
  EXPECT_EQ(proposal_data.nondeterministic_bit_count,
            static_cast<uint32_t>(kChildCap));
  ASSERT_TRUE(proposal_handler.abort_transaction()().has_value());

  // Validation round-trips: A -> B -> kChildCap successful C invocations
  // (each consuming a true bit and bumping depth via
  // note_validation_try_invoke_success); B's frame hits the cap; no bit
  // consumed for B; A's TryInvoke of B fails deterministically; A propagates.
  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
  auto validation_handler =
      make_new_system_call_handler_validation(tx, *ctx, proposal_data);
  auto validation_res = validation_handler.root_invoke()();
  ASSERT_FALSE(validation_res.is_root_success())
      << "Validation must reach the same deterministic failure as proposal";
  EXPECT_EQ(validation_res.result_.error().code(),
            InvokeErrorCode::Deterministic);
  ASSERT_TRUE(validation_handler.abort_transaction()().has_value());
}

// Regression: deterministic root errors must still check that all proposal
// data was consumed.  Before the fix, finalize_after_root_frame() returned
// early for non-success results, skipping check_all_consumed().
TEST_P(ProposeValidateRoundtripTest,
       DeterministicRootErrorSkipsExtraProposalBitCheck)
{
  auto contract_a = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto r = try_invoke_b(ctx, kWriteHello);
        if (!r.has_value())
          co_return InvokeError{InvokeErrorCode::Deterministic};
        if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone)
          co_return InvokeError{InvokeErrorCode::Deterministic};
        co_return InvokeError{InvokeErrorCode::Deterministic};
      });

  auto native_context =
      make_ab_context(std::move(contract_a),
                      make_write_contract({{"b_data0", "hello"}}));
  auto tx = make_invoke_a_tx(0);

  NondeterministicProposalData proposal_data;
  {
    auto proposal_handler = make_new_system_call_handler(tx, *native_context);
    auto proposal_res = proposal_handler.root_invoke()();
    ASSERT_FALSE(proposal_res.is_root_success());
    ASSERT_EQ(proposal_res.result_.error().code(),
              InvokeErrorCode::Deterministic);

    proposal_data = proposal_handler.get_proposal_data();
    ASSERT_EQ(proposal_data.nondeterministic_bit_count, 1U);
    ASSERT_EQ(proposal_data.observed_queue_entry_sequence.size(), 0U);
    ASSERT_NE(proposal_data.nondeterministic_bits[0] & uint8_t{1}, 0);
    ASSERT_EQ(proposal_handler.try_commit_transaction()(),
              ExecResult::ExecResultTxDeterministicError);
  }

  auto tampered = proposal_data;
  tampered.nondeterministic_bit_count = 2;
  tampered.nondeterministic_bits[0] |= uint8_t{1} << 1;

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  auto validation_handler =
      make_new_system_call_handler_validation(tx, *native_context,
                                              std::move(tampered));
  auto validation_res = validation_handler.root_invoke()();
  ASSERT_FALSE(validation_res.is_root_success());

  EXPECT_EQ(validation_handler.try_commit_transaction()(),
            ExecResult::ExecResultTxValidationFailure)
      << "Unused proposal bit must cause validation failure, not deterministic "
         "error";
}

// Regression test for issue #2063: an invalid native contract ID in
// contracts_to_deploy triggers a deterministic error during initial validation,
// returning before check_all_consumed(). Unused proposal bits must still be
// detected as a validation failure.
TEST_P(ProposeValidateRoundtripTest,
       InvalidAttachedNativeContractChecksUnusedProposalBits)
{
  auto native_context = make_noop_context();
  auto tx = make_invoke_a_tx(0);

  Contract invalid_contract(ContractType::kNative);
  invalid_contract.native_contract_id() =
      static_cast<HogexecPrecompiledNativeContracts>(0x7fffffff);
  tx.contracts_to_deploy = {invalid_contract};

  NondeterministicProposalData proposal_data;
  proposal_data.nondeterministic_bit_count = 1;
  proposal_data.nondeterministic_bits = {uint8_t{1}};

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  auto handler =
      make_new_system_call_handler_validation(tx, *native_context,
                                              std::move(proposal_data));
  auto res = handler.root_invoke()();
  ASSERT_FALSE(res.is_root_success());

  EXPECT_EQ(handler.try_commit_transaction()(),
            ExecResult::ExecResultTxValidationFailure)
      << "Unused proposal bit must cause validation failure even when initial "
         "validation rejects an invalid native contract";
}

// Regression test for issue #2063: verify that unused queue entry observations
// are also detected when initial validation fails before root execution.
TEST_P(ProposeValidateRoundtripTest,
       InvalidAttachedContractChecksUnusedQueueObservations)
{
  auto native_context = make_noop_context();
  auto tx = make_invoke_a_tx(0);

  Contract invalid_contract(ContractType::kNative);
  invalid_contract.native_contract_id() =
      static_cast<HogexecPrecompiledNativeContracts>(0x7fffffff);
  tx.contracts_to_deploy = {invalid_contract};

  NondeterministicProposalData proposal_data;
  QueueEntry observation;
  observation.push_back(42);
  proposal_data.observed_queue_entry_sequence = {observation};

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  auto handler =
      make_new_system_call_handler_validation(tx, *native_context,
                                              std::move(proposal_data));
  auto res = handler.root_invoke()();
  ASSERT_FALSE(res.is_root_success());

  EXPECT_EQ(handler.try_commit_transaction()(),
            ExecResult::ExecResultTxValidationFailure)
      << "Unused queue entry observation must cause validation failure even "
         "when initial validation rejects an invalid attached contract";
}

// Regression test for issue #2063: when initial validation itself reports
// ValidationFailure (malformed proposal shape), adding extra unused data
// should still result in ValidationFailure, not get downgraded.
TEST_P(ProposeValidateRoundtripTest,
       MalformedProposalShapeWithExtraDataStillValidationFailure)
{
  auto native_context = make_noop_context();
  auto tx = make_invoke_a_tx(0);

  NondeterministicProposalData malformed;
  malformed.nondeterministic_bit_count = 10;
  malformed.nondeterministic_bits.clear();

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  auto handler = make_new_system_call_handler_validation(tx, *native_context,
                                                         std::move(malformed));
  auto res = handler.root_invoke()();
  ASSERT_FALSE(res.is_root_success());
  EXPECT_EQ(res.result_.error().code(), InvokeErrorCode::ValidationFailure);
  EXPECT_EQ(handler.try_commit_transaction()(),
            ExecResult::ExecResultTxValidationFailure);
}

TEST_P(ProposeValidateRoundtripTest, FinalValidationDoesNotMaskFatalErrors)
{
  auto tx = make_invoke_a_tx(0);

  auto run_case = [this, &tx](InvokeErrorCode root_error,
                              ExecResult expected_result) {
    auto make_error_context = [root_error] {
      auto contract_a =
          make_contract([root_error](NativeSyscallContext & /*unused*/) noexcept
                            -> x23::Async<InvokeStatus<void>> {
            co_return InvokeError{root_error};
          });
      return make_ab_context(std::move(contract_a), make_contract());
    };

    {
      auto ctx = make_error_context();
      NondeterministicProposalData empty_proposal;
      empty_proposal.nondeterministic_bit_count = 0;
      empty_proposal.nondeterministic_bits.clear();
      empty_proposal.observed_queue_entry_sequence.clear();

      discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

      auto handler =
          make_new_system_call_handler_validation(tx, *ctx,
                                                  std::move(empty_proposal));
      auto res = handler.root_invoke()();
      ASSERT_FALSE(res.is_root_success());
      ASSERT_EQ(res.result_.error().code(), root_error);
      EXPECT_EQ(handler.try_commit_transaction()(), expected_result);
    }

    {
      auto ctx = make_error_context();
      NondeterministicProposalData unused_bit;
      unused_bit.nondeterministic_bit_count = 1;
      unused_bit.nondeterministic_bits = {0};

      discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

      auto handler =
          make_new_system_call_handler_validation(tx, *ctx,
                                                  std::move(unused_bit));
      auto res = handler.root_invoke()();
      ASSERT_FALSE(res.is_root_success());
      ASSERT_EQ(res.result_.error().code(), root_error);
      EXPECT_EQ(handler.try_commit_transaction()(), expected_result);
    }
  };

  run_case(InvokeErrorCode::Abandon, ExecResult::ExecResultSystemError);
  run_case(InvokeErrorCode::Unrecoverable, ExecResult::ExecResultUnrecoverable);
}

// ===================================================================
// RCQ observation validation at commit time
//
// When a TX observes concurrent RCQ entries (peek or consume) but the
// final delta for that key is not an RCQ delta (e.g. overwritten with
// a bytestring or deleted), try_acquire_frame_locks() returns false
// and try_commit_transaction() yields ExecResultTxNonDeterministicError.
//
// Observations only arise from *concurrent* entries (inserted by
// another TX in the same block), so these tests insert in TX_A and
// then operate in TX_B within the same block.
// ===================================================================

// TX_A inserts {10} into "rcq0" (same block).  TX_B consumes the
// concurrent entry then overwrites the key with a bytestring.
// Observation key has a bytestring delta, not RCQ → conflict.
TEST_P(ProposeValidateRoundtripTest, ConsumeThenBytestringOverwriteConflicts)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  auto ctx = make_rcq_context();

  // TX_A: insert entry {10} into rcq0, commit (same block).
  auto tx_insert = make_rcq_insert_tx({10});
  NondeterministicProposalData insert_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // TX_B: consume concurrent entry, then overwrite with bytestring.
  auto consume_overwrite_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto res = ctx.handler().retrocausal_queue_consume(key, false)();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        // Overwrite with a bytestring
        uint8_t data[] = {0xAB};
        auto ws =
            ctx.handler().set_bytestring(key, std::as_bytes(std::span(data)),
                                         std::to_underlying(
                                             StorageDeltaFlags::None))();
        if (!ws.has_value())
          co_return ws.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(0, std::move(consume_overwrite_contract));
  auto native_ctx = std::make_unique<NativeContext>(std::move(cmap));

  auto tx_b = make_invoke_a_tx(0);

  // Propose TX_B (abort — we only need proposal data).
  auto proposal_b = propose(tx_b, *native_ctx, false);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Validate: replay TX_A (insert), then TX_B.
  {
    auto ha =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  auto handler = make_new_system_call_handler_validation(tx_b, *native_ctx,
                                                         std::move(proposal_b));
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  EXPECT_EQ(handler.try_commit_transaction()(),
            ExecResult::ExecResultTxNonDeterministicError)
      << "Observation on key overwritten with bytestring should conflict";
}

// TX_A inserts {10} into "rcq0" (same block).  TX_B consumes the
// concurrent entry then deletes the key.
// Observation key has a delete delta, not RCQ → conflict.
TEST_P(ProposeValidateRoundtripTest, ConsumeThenDeleteConflicts)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  auto ctx = make_rcq_context();

  // TX_A: insert entry {10}, commit.
  auto tx_insert = make_rcq_insert_tx({10});
  NondeterministicProposalData insert_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // TX_B: consume concurrent entry, then delete.
  auto consume_delete_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto res = ctx.handler().retrocausal_queue_consume(key, false)();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        auto del =
            ctx.handler().delete_object(key, std::to_underlying(
                                                 StorageDeltaFlags::None))();
        if (!del.has_value())
          co_return del.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(0, std::move(consume_delete_contract));
  auto native_ctx = std::make_unique<NativeContext>(std::move(cmap));

  auto tx_b = make_invoke_a_tx(0);
  auto proposal_b = propose(tx_b, *native_ctx, false);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto ha =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  auto handler = make_new_system_call_handler_validation(tx_b, *native_ctx,
                                                         std::move(proposal_b));
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  EXPECT_EQ(handler.try_commit_transaction()(),
            ExecResult::ExecResultTxNonDeterministicError)
      << "Observation on key that was deleted should conflict";
}

// TX_A inserts {10} into "rcq0" (same block).  TX_B peeks at the
// concurrent entry then consumes it.  The observation is covered by
// current_block_consumed → success.
TEST_P(ProposeValidateRoundtripTest, PeekThenConsumeSucceeds)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  auto ctx = make_rcq_context();

  // TX_A: insert entry {10}, commit.
  auto tx_insert = make_rcq_insert_tx({10});
  NondeterministicProposalData insert_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // TX_B: peek then consume.
  auto peek_then_consume_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto peek_res = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!peek_res.has_value())
          co_return peek_res.error().to_invoke_error();
        auto consume_res =
            ctx.handler().retrocausal_queue_consume(key, false)();
        if (!consume_res.has_value())
          co_return consume_res.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(0, std::move(peek_then_consume_contract));
  auto native_ctx = std::make_unique<NativeContext>(std::move(cmap));

  auto tx_b = make_invoke_a_tx(0);
  auto proposal_b = propose(tx_b, *native_ctx, true);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto ha =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  auto handler = make_new_system_call_handler_validation(tx_b, *native_ctx,
                                                         std::move(proposal_b));
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  EXPECT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK)
      << "Peek followed by consume should succeed — observation is covered";
}

// TX_A inserts {10} into "rcq0" (same block).  TX_B only peeks at the
// concurrent entry, does nothing else.  An observation-only RCQ delta is
// emitted for the key (not in modified_items_) and validated by HogDb.
TEST_P(ProposeValidateRoundtripTest, PeekOnlyWithoutWriteSucceeds)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  auto ctx = make_rcq_context();

  // TX_A: insert entry {10}, commit.
  auto tx_insert = make_rcq_insert_tx({10});
  NondeterministicProposalData insert_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // TX_B: peek only, no write.
  auto peek_only_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto res = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(0, std::move(peek_only_contract));
  auto native_ctx = std::make_unique<NativeContext>(std::move(cmap));

  auto tx_b = make_invoke_a_tx(0);
  auto proposal_b = propose(tx_b, *native_ctx, false);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto ha =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  auto handler = make_new_system_call_handler_validation(tx_b, *native_ctx,
                                                         std::move(proposal_b));
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  EXPECT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK)
      << "Peek-only observation with no write to the key should succeed — "
         "unmatched observations are harmless if no delta is emitted";
}

// Two-contract test: TX_A inserts {10} (same block).  In TX_B,
// contract B consumes from "rcq0" then faults (frame reverts).
// Contract A catches the fault via try_invoke, then re-consumes from
// "rcq0" itself.  The re-consume in A succeeds → commit should succeed.
TEST_P(ProposeValidateRoundtripTest, ConsumeInRevertedFrameReconsumeSucceeds)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  auto ctx = make_rcq_context();

  // TX_A: insert entry {10}, commit.
  auto tx_insert = make_rcq_insert_tx({10});
  NondeterministicProposalData insert_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // TX_B: A try_invokes B (which consumes + faults), then A re-consumes.
  auto contract_b = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto res = ctx.handler().retrocausal_queue_consume(key, false)();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        // Fault — frame will be reverted
        co_return InvokeError{InvokeErrorCode::Deterministic};
      });
  auto contract_a = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        // try_invoke B method 0 — it will fault
        auto r = try_invoke_b(ctx, 0);
        if (!r.has_value())
          co_return InvokeError{InvokeErrorCode::Deterministic};
        // B faulted, frame reverted. Now A re-consumes from rcq0.
        auto key = make_storage_key("rcq0");
        auto res = ctx.handler().retrocausal_queue_consume(key, false)();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });

  auto native_ctx =
      make_ab_context(std::move(contract_a), std::move(contract_b));
  auto tx_b = make_invoke_a_tx(0);
  auto proposal_b = propose(tx_b, *native_ctx, true);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto ha =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  auto handler = make_new_system_call_handler_validation(tx_b, *native_ctx,
                                                         std::move(proposal_b));
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  EXPECT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK)
      << "Re-consume after reverted frame should succeed";
}

// Two-contract test: TX_A inserts {10} (same block).  In TX_B,
// contract B consumes from "rcq0" then faults (frame reverts).
// Contract A catches the fault via try_invoke but does NOT re-consume.
// The child frame's consume probe sees no concurrent entry (bit=false),
// so no observation is generated, and the transaction commits cleanly.
// Contrast with ConsumeInRevertedFrameReconsumeSucceeds where A's
// subsequent root-frame re-consume DOES observe the concurrent entry.
TEST_P(ProposeValidateRoundtripTest,
       ConsumeInRevertedFrameNoReconsumeCommitsCleanly)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  auto ctx = make_rcq_context();

  // TX_A: insert entry {10}, commit.
  auto tx_insert = make_rcq_insert_tx({10});
  NondeterministicProposalData insert_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // TX_B: A try_invokes B (which consumes + faults), A does nothing else.
  auto contract_b = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto res = ctx.handler().retrocausal_queue_consume(key, false)();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        // Fault — frame will be reverted
        co_return InvokeError{InvokeErrorCode::Deterministic};
      });
  auto contract_a = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        // try_invoke B method 0 — it will fault
        auto r = try_invoke_b(ctx, 0);
        if (!r.has_value())
          co_return InvokeError{InvokeErrorCode::Deterministic};
        // Do NOT re-consume — no observation generated in reverted child
        co_return InvokeStatus<void>{};
      });

  auto native_ctx =
      make_ab_context(std::move(contract_a), std::move(contract_b));
  auto tx_b = make_invoke_a_tx(0);
  auto proposal_b = propose(tx_b, *native_ctx, false);

  // The child frame's concurrent consume probe sees no concurrent entry
  // (the child try_invoke runs in an environment where the concurrent
  // entry is not yet visible to the probe). This produces bit=false with
  // no observation.
  EXPECT_EQ(proposal_b.nondeterministic_bit_count, 1U);
  EXPECT_EQ(proposal_b.observed_queue_entry_sequence.size(), 0U);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto ha =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  auto handler = make_new_system_call_handler_validation(tx_b, *native_ctx,
                                                         std::move(proposal_b));
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  EXPECT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK)
      << "No observation generated — reverted child saw no concurrent entry";
}

// TX_A inserts {10} into "rcq0" (same block).  TX_B peeks at the
// concurrent entry then overwrites the key with a bytestring.
// Peek without consume means retrocausally_consumed_count=0, so
// subspan keeps the full observation.  Type conflict fires.
TEST_P(ProposeValidateRoundtripTest, PeekThenBytestringOverwriteConflicts)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  auto ctx = make_rcq_context();

  // TX_A: insert entry {10}, commit.
  auto tx_insert = make_rcq_insert_tx({10});
  NondeterministicProposalData insert_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // TX_B: peek at concurrent entry, then overwrite with bytestring.
  auto peek_overwrite_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto peek_res = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!peek_res.has_value())
          co_return peek_res.error().to_invoke_error();
        // Overwrite with a bytestring
        uint8_t data[] = {0xAB};
        auto ws =
            ctx.handler().set_bytestring(key, std::as_bytes(std::span(data)),
                                         std::to_underlying(
                                             StorageDeltaFlags::None))();
        if (!ws.has_value())
          co_return ws.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(0, std::move(peek_overwrite_contract));
  auto native_ctx = std::make_unique<NativeContext>(std::move(cmap));

  auto tx_b = make_invoke_a_tx(0);
  auto proposal_b = propose(tx_b, *native_ctx, false);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto ha =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  auto handler = make_new_system_call_handler_validation(tx_b, *native_ctx,
                                                         std::move(proposal_b));
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  EXPECT_EQ(handler.try_commit_transaction()(),
            ExecResult::ExecResultTxNonDeterministicError)
      << "Peek observation on key overwritten with bytestring should conflict";
}

// TX_A inserts {10} into "rcq0" (same block).  TX_B peeks at the
// concurrent entry then overwrites the key with set_nnint.
// Same conflict as PeekThenBytestringOverwriteConflicts but via NNInt.
TEST_P(ProposeValidateRoundtripTest, PeekThenNNIntOverwriteConflicts)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  auto ctx = make_rcq_context();

  // TX_A: insert entry {10}, commit.
  auto tx_insert = make_rcq_insert_tx({10});
  NondeterministicProposalData insert_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // TX_B: peek at concurrent entry, then overwrite with nnint.
  auto peek_nnint_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto peek_res = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!peek_res.has_value())
          co_return peek_res.error().to_invoke_error();
        // Overwrite with set_nnint(set_value=0, add_value=42)
        auto ws = ctx.handler().set_nnint(key, std::optional<int128_t>{0},
                                          int128_t{42},
                                          std::to_underlying(
                                              StorageDeltaFlags::None))();
        if (!ws.has_value())
          co_return ws.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(0, std::move(peek_nnint_contract));
  auto native_ctx = std::make_unique<NativeContext>(std::move(cmap));

  auto tx_b = make_invoke_a_tx(0);
  auto proposal_b = propose(tx_b, *native_ctx, false);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto ha =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  auto handler = make_new_system_call_handler_validation(tx_b, *native_ctx,
                                                         std::move(proposal_b));
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  EXPECT_EQ(handler.try_commit_transaction()(),
            ExecResult::ExecResultTxNonDeterministicError)
      << "Peek observation on key overwritten with nnint should conflict";
}

// TX_A inserts {10} and {20} into "rcq0" (same block).
// TX_B consumes one entry then peeks the next.
// The consumed entry goes into current_block_consumed; the peeked entry
// goes into observed_concurrent_entries.  Both should succeed.
TEST_P(ProposeValidateRoundtripTest, ConsumeThenPeekSameKey)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  auto ctx = make_rcq_context();

  // TX_A: insert entries {10} and {20}, commit.
  auto tx_insert = make_rcq_insert_tx({10, 20});
  NondeterministicProposalData insert_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // TX_B: consume one, then peek the next.
  auto consume_then_peek_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto consume_res =
            ctx.handler().retrocausal_queue_consume(key, false)();
        if (!consume_res.has_value())
          co_return consume_res.error().to_invoke_error();
        auto peek_res = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!peek_res.has_value())
          co_return peek_res.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(0, std::move(consume_then_peek_contract));
  auto native_ctx = std::make_unique<NativeContext>(std::move(cmap));

  auto tx_b = make_invoke_a_tx(0);
  auto proposal_b = propose(tx_b, *native_ctx, true);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto ha =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  auto handler = make_new_system_call_handler_validation(tx_b, *native_ctx,
                                                         std::move(proposal_b));
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  EXPECT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK)
      << "Consume then peek on same key should succeed — consumed entry in "
         "current_block_consumed, peeked entry in observed_concurrent_entries";
}

// TX_B inserts {42} into "rcq0" then immediately consumes from "rcq0".
// The consume gets the self-inserted entry (step 2 in get_next_item),
// never reaching concurrent entries (step 3).  No nondeterministic bits
// or observations should be generated.
TEST_P(ProposeValidateRoundtripTest, SelfInsertSelfConsumeNoObservation)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  // Single TX: insert then consume from the same key.
  auto self_insert_consume_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        uint8_t val = 42;
        auto ins_res = ctx.handler().retrocausal_queue_insert(
            key, std::as_bytes(std::span(&val, 1)))();
        if (!ins_res.has_value())
          co_return ins_res.error().to_invoke_error();
        auto consume_res =
            ctx.handler().retrocausal_queue_consume(key, false)();
        if (!consume_res.has_value())
          co_return consume_res.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(0, std::move(self_insert_consume_contract));
  auto native_ctx = std::make_unique<NativeContext>(std::move(cmap));

  auto tx_b = make_invoke_a_tx(0);
  auto proposal_b = propose(tx_b, *native_ctx, true);

  // No nondeterministic bits — self-insert consumed locally.
  EXPECT_EQ(proposal_b.nondeterministic_bit_count, 0U);
  EXPECT_EQ(proposal_b.observed_queue_entry_sequence.size(), 0U);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  auto handler = make_new_system_call_handler_validation(tx_b, *native_ctx,
                                                         std::move(proposal_b));
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  EXPECT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK)
      << "Self-insert then self-consume should succeed with no observations";
}

// TX_A inserts {10}.  TX_B peeks at "rcq0" (gets {10} in proposal).
// We tamper the proposal: replace observed_queue_entry_sequence[0] with
// {0xFF}.  In validation, TX_B's peek returns the forged {0xFF}.  The
// observation-only delta records observed_concurrent_entries={0xFF}.
// Block close: HogDb checks that every observed entry was actually
// inserted by some TX.  {0xFF} was never inserted → reject_block.
TEST_P(ProposeValidateRoundtripTest, ForgedObservationRejectedEndToEnd)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  auto ctx = make_rcq_context();

  // TX_A: insert entry {10}, commit.
  auto tx_insert = make_rcq_insert_tx({10});
  NondeterministicProposalData insert_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // TX_B: peek only (no consume, no write).
  auto peek_only_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto res = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(0, std::move(peek_only_contract));
  auto native_ctx = std::make_unique<NativeContext>(std::move(cmap));

  auto tx_b = make_invoke_a_tx(0);
  auto proposal_b = propose(tx_b, *native_ctx, false);

  // Tamper: replace observed entry {10} with {0xFF}.
  ASSERT_GE(proposal_b.observed_queue_entry_sequence.size(), 1U);
  proposal_b.observed_queue_entry_sequence[0] = {0xFF};

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Validate: replay TX_A (insert {10}), then TX_B with tampered proposal.
  {
    auto ha =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  auto handler = make_new_system_call_handler_validation(tx_b, *native_ctx,
                                                         std::move(proposal_b));
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  // try_commit succeeds — the executor does not validate observation content.
  EXPECT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);

  // Block close: HogDb detects that {0xFF} was never inserted → reject.
  reject_block(BlockExecutionMode::kBlockExecutionModeValidation);
}

// Two-contract test: TX_A inserts {10} into "rcq0" under kAddrB.
// TX_B: contract A try_invokes B (which peeks at kAddrB's "rcq0"),
// B returns success.  The peek observation at kAddrB is logged in the
// child frame and promoted to the parent on success.  Commit succeeds.
TEST_P(ProposeValidateRoundtripTest, PeekInSuccessfulChildFrame)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  // First, insert {10} into kAddrB's "rcq0" via a simple insert contract.
  auto insert_contract_b = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        uint8_t val = 10;
        auto res = ctx.handler().retrocausal_queue_insert(
            key, std::as_bytes(std::span(&val, 1)))();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  auto insert_contract_a =
      make_contract([](NativeSyscallContext & /*ctx*/) noexcept
                        -> x23::Async<InvokeStatus<void>> {
        co_return InvokeStatus<void>{};
      });
  auto insert_ctx = make_ab_context(std::move(insert_contract_a),
                                    std::move(insert_contract_b));

  // TX_insert targets B to insert {10} at kAddrB.
  auto tx_insert =
      Transaction{.invocation =
                      Invocation{.invoked_address = kAddrB,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()};

  NondeterministicProposalData insert_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *insert_ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // TX_B: A try_invokes B (which peeks at kAddrB's "rcq0"), B succeeds.
  auto peek_contract_b = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto res = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  auto invoke_contract_a = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto r = try_invoke_b(ctx, 0);
        if (!r.has_value())
          co_return InvokeError{InvokeErrorCode::Deterministic};
        co_return InvokeStatus<void>{};
      });

  auto native_ctx =
      make_ab_context(std::move(invoke_contract_a), std::move(peek_contract_b));
  auto tx_b = make_invoke_a_tx(0);
  auto proposal_b = propose(tx_b, *native_ctx, false);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Validate: replay insert at kAddrB, then TX_B.
  {
    auto ha = make_new_system_call_handler_validation(tx_insert, *insert_ctx,
                                                      insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  auto handler = make_new_system_call_handler_validation(tx_b, *native_ctx,
                                                         std::move(proposal_b));
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  EXPECT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK)
      << "Peek observation from successful child frame should be promoted "
         "to parent and commit cleanly";
}

// Block 1: insert {10} into "rcq0", advance_block.
// Block 2: TX_A inserts {20} (concurrent).  TX_B consumes from "rcq0"
// (gets {10} from past block), then peeks (gets {20} from concurrent).
// Past-block consume does not generate an observation; the peek does.
// Both should succeed.
TEST_P(ProposeValidateRoundtripTest, PastBlockConsumeThenPeekConcurrent)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  auto ctx = make_rcq_context();

  // Block 1: insert {10}, commit, advance.
  {
    auto tx_insert = make_rcq_insert_tx({10});
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  advance_block();

  // Block 2: TX_A inserts {20} (concurrent in this block).
  auto tx_insert_20 = make_rcq_insert_tx({20});
  NondeterministicProposalData insert_20_p;
  {
    auto ha = make_new_system_call_handler(tx_insert_20, *ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_20_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // TX_B: consume one (gets past-block {10}), peek next (gets concurrent {20}).
  auto consume_peek_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto consume_res =
            ctx.handler().retrocausal_queue_consume(key, false)();
        if (!consume_res.has_value())
          co_return consume_res.error().to_invoke_error();
        auto peek_res = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!peek_res.has_value())
          co_return peek_res.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(0, std::move(consume_peek_contract));
  auto native_ctx = std::make_unique<NativeContext>(std::move(cmap));

  auto tx_b = make_invoke_a_tx(0);
  auto proposal_b = propose(tx_b, *native_ctx, true);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Validate: replay TX_A (insert {20}), then TX_B.
  {
    auto ha = make_new_system_call_handler_validation(tx_insert_20, *ctx,
                                                      insert_20_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  auto handler = make_new_system_call_handler_validation(tx_b, *native_ctx,
                                                         std::move(proposal_b));
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  EXPECT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK)
      << "Past-block consume then concurrent peek should both succeed";
}

// Frame revert with concurrent observation: A try_invokes B. B consumes a
// concurrent RCQ entry at kAddrB and faults. The observation persists in
// queue_entry_observations_. Root A does nothing with that key. The
// observation must be emitted as an observation-only delta for kAddrB.
TEST_P(ProposeValidateRoundtripTest,
       ConsumeInRevertedFrameWithConcurrentEntryEmitsObservation)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  // B: method 0 = consume from "rcq0" then fault.
  auto b_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto res = ctx.handler().retrocausal_queue_consume(key, false)();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        co_return InvokeError{InvokeErrorCode::Deterministic};
      });

  // A: method 0 = try_invoke B.0 (faults), return success.
  auto a_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto r = try_invoke_b(ctx, 0);
        if (!r.has_value())
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        co_return InvokeStatus<void>{};
      });

  auto ctx = make_ab_context(std::move(a_contract), std::move(b_contract));

  // Insert at kAddrB (same block → concurrent entry visible to B).
  auto b_insert_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        uint8_t val = 10;
        auto res = ctx.handler().retrocausal_queue_insert(
            key, std::as_bytes(std::span(&val, 1)))();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> insert_cmap;
  insert_cmap.emplace(1, std::move(b_insert_contract));
  auto insert_ctx = std::make_unique<NativeContext>(std::move(insert_cmap));

  auto tx_insert =
      Transaction{.invocation =
                      Invocation{.invoked_address = kAddrB,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()};
  auto insert_p = propose(tx_insert, *insert_ctx);

  auto tx = make_invoke_a_tx(0);
  auto proposal = propose(tx, *ctx, /*commit=*/false);

  // B consumed a concurrent entry → 1 bit (true), 1 observation.
  // B faulted deterministically → no lock-acquisition bit.
  ASSERT_EQ(proposal.nondeterministic_bit_count, 1U);
  ASSERT_EQ(proposal.observed_queue_entry_sequence.size(), 1U);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Validate: replay insert first, then the reverted-frame TX.
  {
    auto ha = make_new_system_call_handler_validation(tx_insert, *insert_ctx,
                                                      insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  auto handler =
      make_new_system_call_handler_validation(tx, *ctx, std::move(proposal));
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK)
      << "Reverted child frame observation should be emitted as an "
         "observation-only delta and validated by HogDb";
  advance_block();
}

// Malicious proposer scenario: contract A try_invokes B twice.
// Frame 1: B consumes a concurrent RCQ entry, then faults (observation
// logged, frame reverts).  Frame 2: B writes a bytestring to the same key.
// In proposal mode, Frame 2's lock acquisition fails (RCQ/bytestring type
// conflict), so the nondeterministic bit is false.  A malicious proposer
// doctors the bit to true.  In validation mode, finish_frame must detect
// the observation/type mismatch and return ValidationFailure.
TEST_P(ProposeValidateRoundtripTest,
       DoctoredTryInvokeBitOnObservedKeyReturnsValidationFailure)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  // B: method 0 = consume from "rcq0" then fault.
  //    method 1 = write bytestring to "rcq0".
  auto b_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto res = ctx.handler().retrocausal_queue_consume(key, false)();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        co_return InvokeError{InvokeErrorCode::Deterministic};
      },
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto overwrite = std::string_view("overwritten");
        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), key, std::as_bytes(std::span(overwrite)),
            std::to_underlying(StorageDeltaFlags::None))();
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone)
          co_return InvokeError{InvokeErrorCode::Deterministic};
        co_return InvokeStatus<void>{};
      });

  // A: method 0 = try_invoke B.0 (faults), then try_invoke B.1.
  auto a_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        // Frame 1: B.0 consumes + faults
        auto r1 = try_invoke_b(ctx, 0);
        if (!r1.has_value())
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        // Frame 2: B.1 writes bytestring
        auto r2 = try_invoke_b(ctx, 1);
        if (!r2.has_value())
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        co_return InvokeStatus<void>{};
      });

  auto ctx = make_ab_context(std::move(a_contract), std::move(b_contract));

  // Insert at kAddrB so B's consume can find the concurrent entry.
  auto b_insert_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        uint8_t val = 10;
        auto res = ctx.handler().retrocausal_queue_insert(
            key, std::as_bytes(std::span(&val, 1)))();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> insert_cmap;
  insert_cmap.emplace(1, std::move(b_insert_contract));
  auto insert_ctx = std::make_unique<NativeContext>(std::move(insert_cmap));

  // TX_insert: insert {10} at kAddrB — same block, so it's a concurrent entry.
  auto tx_insert =
      Transaction{.invocation =
                      Invocation{.invoked_address = kAddrB,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()};
  propose(tx_insert, *insert_ctx);

  // Same block: propose the A→B.0(fault)→B.1(bytestring) TX.
  auto tx = make_invoke_a_tx(0);
  auto proposal = propose(tx, *ctx, /*commit=*/false);

  // Bit 0 = true (concurrent entry found in B.0's consume probe).
  // Frame 1 faults deterministically → no lock-acquisition bit.
  // Bit 1 = false (Frame 2 lock acquisition: RCQ/bytestring type conflict).
  ASSERT_EQ(proposal.nondeterministic_bit_count, 2U);
  ASSERT_EQ(proposal.observed_queue_entry_sequence.size(), 1U);
  ASSERT_EQ(proposal.nondeterministic_bits[0] & (1 << 1), 0)
      << "Bit 1 should be false (Frame 2 lock acquisition failed)";

  // Doctor: flip bit 1 to true, claiming Frame 2's lock succeeded.
  auto doctored = proposal;
  doctored.nondeterministic_bits[0] |= (1 << 1);

  // Re-propose the insert to recapture its proposal data. This is a second
  // commit of the insert in the same proposal block, so it needs its own
  // uniquifier (the replay cache rejects a duplicate within a block).
  tx_insert.uniquifier = make_new_uniquifier();
  auto insert_p = propose(tx_insert, *insert_ctx);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Replay doctored TX first — should fail at root_invoke.
  auto handler =
      make_new_system_call_handler_validation(tx, *ctx, std::move(doctored));
  auto res = handler.root_invoke()();
  EXPECT_FALSE(res.is_root_success())
      << "finish_frame should detect observation/type mismatch when the "
         "doctored bit claims Frame 2 succeeded";

  // Replay the insert TX after (in the same validation block).
  auto insert_handler =
      make_new_system_call_handler_validation(tx_insert, *insert_ctx, insert_p);
  EXPECT_TRUE(insert_handler.root_invoke()().is_root_success());
  EXPECT_EQ(insert_handler.try_commit_transaction()(),
            ExecResult::ExecResultOK);
}

// Malicious proposer scenario: contract A try_invokes B.  B peeks at a
// concurrent RCQ entry and returns success.  In proposal mode, the lock
// acquisition bit is true.  A malicious proposer doctors it to false,
// causing the child frame to be treated as failed.  The observation was
// consumed from the proposal stream (nondeterministic bit + observation)
// but the frame's writes are discarded — no observation delta is emitted.
// The block close should still catch this: the observation entry has
// observe_count > 0 in HogDb but was never matched by an insert visible
// to the observer's delta... actually, the observation-only delta is NOT
// emitted because the frame reverted.  However, check_all_consumed()
// should still pass (bits and observations were consumed from the stream).
// The real question is whether the block finalizes without catching the
// suppressed observation.
TEST_P(ProposeValidateRoundtripTest, DoctoredTryInvokeBitFalseDropsObservation)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  // B: method 0 = peek at "rcq0", return success.
  auto b_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto res = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });

  // A: method 0 = try_invoke B.0.
  auto a_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto r = try_invoke_b(ctx, 0);
        if (!r.has_value())
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        co_return InvokeStatus<void>{};
      });

  auto ctx = make_ab_context(std::move(a_contract), std::move(b_contract));

  // Insert at kAddrB (same block, concurrent entry for B's peek).
  auto b_insert_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        uint8_t val = 10;
        auto res = ctx.handler().retrocausal_queue_insert(
            key, std::as_bytes(std::span(&val, 1)))();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> insert_cmap;
  insert_cmap.emplace(1, std::move(b_insert_contract));
  auto insert_ctx = std::make_unique<NativeContext>(std::move(insert_cmap));

  auto tx_insert =
      Transaction{.invocation =
                      Invocation{.invoked_address = kAddrB,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()};
  auto insert_p = propose(tx_insert, *insert_ctx);

  // Same block: A try_invokes B (peek succeeds).
  auto tx = make_invoke_a_tx(0);
  auto proposal = propose(tx, *ctx);

  // Bit 0 = true (concurrent entry found in B's peek probe).
  // Bit 1 = true (B's frame lock acquisition succeeded — peek only, no
  //          conflict).
  ASSERT_EQ(proposal.nondeterministic_bit_count, 2U);
  ASSERT_EQ(proposal.observed_queue_entry_sequence.size(), 1U);

  // Doctor: flip bit 1 from true to false (claim frame failed).
  auto doctored = proposal;
  doctored.nondeterministic_bits[0] &= ~(1 << 1);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Replay insert.
  {
    auto ha = make_new_system_call_handler_validation(tx_insert, *insert_ctx,
                                                      insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Replay with doctored proposal.
  auto handler =
      make_new_system_call_handler_validation(tx, *ctx, std::move(doctored));
  auto res = handler.root_invoke()();
  // Root invoke succeeds — the doctored bit just makes the frame "fail",
  // which is handled normally (frame reverted, A continues).
  ASSERT_TRUE(res.is_root_success());
  // try_commit succeeds — the observation persists in queue_entry_observations_
  // even though the frame reverted. try_acquire_frame_locks emits an
  // observation-only RCQ delta for kAddrB (key not in modified_items_).
  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  // Block close succeeds because HogDb validates the observation against
  // the concurrent insert from tx_insert. If the insert were missing,
  // stage_committed_changes would reject (observe_count > insert_count).
  advance_block();
}

// Same as DoctoredTryInvokeBitFalseDropsObservation, but the concurrent
// insert is NOT replayed in validation mode.  The observation-only delta
// reaches HogDb with no matching insert → block rejected.
TEST_P(ProposeValidateRoundtripTest,
       DoctoredTryInvokeBitFalseNoInsertRejectsBlock)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  auto b_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto res = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });

  auto a_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto r = try_invoke_b(ctx, 0);
        if (!r.has_value())
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        co_return InvokeStatus<void>{};
      });

  auto ctx = make_ab_context(std::move(a_contract), std::move(b_contract));

  auto b_insert_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        uint8_t val = 10;
        auto res = ctx.handler().retrocausal_queue_insert(
            key, std::as_bytes(std::span(&val, 1)))();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> insert_cmap;
  insert_cmap.emplace(1, std::move(b_insert_contract));
  auto insert_ctx = std::make_unique<NativeContext>(std::move(insert_cmap));

  auto tx_insert =
      Transaction{.invocation =
                      Invocation{.invoked_address = kAddrB,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()};
  propose(tx_insert, *insert_ctx);

  auto tx = make_invoke_a_tx(0);
  auto proposal = propose(tx, *ctx);

  ASSERT_EQ(proposal.nondeterministic_bit_count, 2U);
  ASSERT_EQ(proposal.observed_queue_entry_sequence.size(), 1U);

  auto doctored = proposal;
  doctored.nondeterministic_bits[0] &= ~(1 << 1);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Do NOT replay the insert TX. Only replay the doctored TX.
  auto handler =
      make_new_system_call_handler_validation(tx, *ctx, std::move(doctored));
  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);

  // Block close rejects: observation-only delta has observe_count=1 for
  // entry {10} but insert_count=0 (no insert TX in this validation block).
  reject_block(BlockExecutionMode::kBlockExecutionModeValidation);
  advance_block();
}

// Three reverted child frames each observe the same concurrent entry.
// Because each child frame starts with retrocausally_consumed_count_=0
// (parent's value is restored on revert), all three observe at index 0
// and hit the same cached entry in queue_entry_observations_.  The
// observation count should be 1 (not 3), so a single insert suffices.
TEST_P(ProposeValidateRoundtripTest, RepeatedRevertedObservationsCountAsOne)
{
  if (GetParam() != TestDB::kHogDb)
    GTEST_SKIP() << "RCQ observation validation requires kHogDb";

  // B: method 0 = consume from "rcq0" then fault.
  auto b_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto res = ctx.handler().retrocausal_queue_consume(key, false)();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        co_return InvokeError{InvokeErrorCode::Deterministic};
      });

  // A: method 0 = try_invoke B.0 three times, all fault.
  auto a_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto r1 = try_invoke_b(ctx, 0);
        if (!r1.has_value())
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        auto r2 = try_invoke_b(ctx, 0);
        if (!r2.has_value())
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        auto r3 = try_invoke_b(ctx, 0);
        if (!r3.has_value())
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        co_return InvokeStatus<void>{};
      });

  auto ctx = make_ab_context(std::move(a_contract), std::move(b_contract));

  // Insert one entry at kAddrB (same block → concurrent).
  auto b_insert_contract = make_contract(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        uint8_t val = 10;
        auto res = ctx.handler().retrocausal_queue_insert(
            key, std::as_bytes(std::span(&val, 1)))();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> insert_cmap;
  insert_cmap.emplace(1, std::move(b_insert_contract));
  auto insert_ctx = std::make_unique<NativeContext>(std::move(insert_cmap));

  auto tx_insert =
      Transaction{.invocation =
                      Invocation{.invoked_address = kAddrB,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()};
  auto insert_p = propose(tx_insert, *insert_ctx);

  auto tx = make_invoke_a_tx(0);
  auto proposal = propose(tx, *ctx, /*commit=*/false);

  // Each child frame's consume probe produces 1 bit (true = entry found).
  // All three re-observe the same cached entry at index 0.
  // Only 1 unique observation in the sequence.
  ASSERT_EQ(proposal.nondeterministic_bit_count, 3U);
  ASSERT_EQ(proposal.observed_queue_entry_sequence.size(), 1U);

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto ha = make_new_system_call_handler_validation(tx_insert, *insert_ctx,
                                                      insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  auto handler =
      make_new_system_call_handler_validation(tx, *ctx, std::move(proposal));
  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);

  // Block closes: one insert, one observation. observe_count=1 <=
  // insert_count=1.
  advance_block();
}

INSTANTIATE_TEST_SUITE_P(TestDBs, ProposeValidateRoundtripTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
