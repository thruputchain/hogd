#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
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
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// Tests for doctored RCQ proposal data that exploit the independent
// consume_count / observe_count validation in has_unmatched_entries().
// NOLINTNEXTLINE(misc-use-internal-linkage)
class RcqDoctoredProposalDataTest : public ::testing::Test,
                                    public NativeContractTestFramework {
public:
  RcqDoctoredProposalDataTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

protected:
  void SetUp() override
  {
    // kAddrA / kAddrB are the root invoked addresses; every committed
    // transaction consumes a replay-cache entry charged to the invoked
    // address. The worst case in any single test commits 6 transactions
    // against one address (RevertedDoubleConsumePlusHonestConsumePeekIsValid
    // commits 6 against kAddrB), each priced as a single consume since blocks
    // reset the committed snapshot. Top up both grants by that worst-case
    // amount (0 on MockInMemory; this suite always runs HogDb).
    write_contract(native_contract_a_xdr_);
    write_deployment(kAddrA, native_contract_a_xdr_);
    setup_grant_storage_token(kAddrA, 1'000'000'000 +
                                          (replay_cache_cost_charged(1) * 6));

    write_contract(native_contract_b_xdr_);
    write_deployment(kAddrB, native_contract_b_xdr_);
    setup_grant_storage_token(kAddrB, 1'000'000'000 +
                                          (replay_cache_cost_charged(1) * 6));
  }

  static constexpr ContractAddress kAddrA = make_contract_address(0);
  static constexpr ContractAddress kAddrB = make_contract_address(1);

  std::shared_ptr<Contract> native_contract_a_xdr_ = make_native_contract(0);
  std::shared_ptr<Contract> native_contract_b_xdr_ = make_native_contract(1);

  template<typename... Fns>
  static std::unique_ptr<NativeContract> build_contract(Fns &&...fns)
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> map;
    Runtime::MethodID id = 0;
    (map.emplace(id++, std::forward<Fns>(fns)), ...);
    return std::make_unique<NativeContract>(std::move(map));
  }

  static std::unique_ptr<NativeContext>
  make_ab_context(std::unique_ptr<NativeContract> a,
                  std::unique_ptr<NativeContract> b)
  {
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
    cmap.emplace(0, std::move(a));
    cmap.emplace(1, std::move(b));
    return std::make_unique<NativeContext>(std::move(cmap));
  }

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

  static std::unique_ptr<RuntimeContext> make_rcq_insert_context()
  {
    auto contract = build_contract([](NativeSyscallContext &ctx) noexcept
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
    });
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
    cmap.emplace(1, std::move(contract));
    return std::make_unique<NativeContext>(std::move(cmap));
  }

  Transaction make_insert_tx(std::vector<uint8_t> const &values)
  {
    return Transaction{
        .invocation =
            Invocation{.invoked_address = kAddrB,
                       .entrypoint = entrypoint_from_method_id(0),
                       .gas_limit = UINT64_MAX,
                       .calldata = Calldata(values.begin(), values.end())},
        .uniquifier = make_new_uniquifier()};
  }

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

  Transaction make_invoke_b_tx(Runtime::MethodID method)
  {
    return Transaction{.invocation =
                           Invocation{.invoked_address = kAddrB,
                                      .entrypoint =
                                          entrypoint_from_method_id(method),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = {}},
                       .uniquifier = make_new_uniquifier()};
  }
};

// PoC for issue #2357: one insert, then a consume + peek of the same value.
// Honest proposal can't observe {10} twice with one insert, but doctored
// validation data with two present bits and two {10} observations passes
// block close because has_unmatched_entries() checks consume_count and
// observe_count independently against insert_count.
TEST_F(RcqDoctoredProposalDataTest,
       ConsumeThenPeekSameValueWithOneInsertValidationWritesMarker)
{
  auto insert_ctx = make_rcq_insert_context();

  auto tx_insert = make_insert_tx({10});
  NondeterministicProposalData insert_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *insert_ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  bool marker_branch_taken = false;
  auto consume_then_peek_marker_contract =
      build_contract([&marker_branch_taken](NativeSyscallContext &ctx) noexcept
                         -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        QueueEntry expected_entry;
        expected_entry.push_back(10);

        auto consume_res =
            ctx.handler().retrocausal_queue_consume(key, false)();
        if (!consume_res.has_value())
          co_return consume_res.error().to_invoke_error();

        auto peek_res = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!peek_res.has_value())
          co_return peek_res.error().to_invoke_error();

        if (!consume_res.value().has_value() || !peek_res.value().has_value() ||
            consume_res.value().value() != expected_entry ||
            peek_res.value().value() != expected_entry) {
          co_return InvokeStatus<void>{};
        }

        marker_branch_taken = true;
        auto marker_key = make_storage_key("marker");
        uint8_t marker[] = {0xEE};
        auto ws = ctx.handler().set_bytestring(marker_key,
                                               std::as_bytes(std::span(marker)),
                                               std::to_underlying(
                                                   StorageDeltaFlags::None))();
        if (!ws.has_value())
          co_return ws.error().to_invoke_error();
        if (!ws.value().has_value())
          co_return InvokeError{InvokeErrorCode::Deterministic};
        co_return InvokeStatus<void>{};
      });

  // The PoC contract runs at kAddrB (same address as the RCQ inserts).
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(1, std::move(consume_then_peek_marker_contract));
  auto native_ctx = std::make_unique<NativeContext>(std::move(cmap));

  auto tx_b = make_invoke_b_tx(0);
  auto honest_proposal_b = [&] {
    auto ha = make_new_system_call_handler(tx_b, *native_ctx);
    EXPECT_TRUE(ha.root_invoke()().is_root_success());
    auto p = ha.get_proposal_data();
    EXPECT_TRUE(ha.abort_transaction()().has_value());
    return p;
  }();
  ASSERT_FALSE(marker_branch_taken)
      << "Proposal with one inserted entry cannot observe {10} twice";
  ASSERT_EQ(honest_proposal_b.nondeterministic_bit_count, 2U);
  ASSERT_EQ(honest_proposal_b.nondeterministic_bits.size(), 1U);
  EXPECT_EQ(honest_proposal_b.nondeterministic_bits[0], uint8_t{0x01});
  ASSERT_EQ(honest_proposal_b.observed_queue_entry_sequence.size(), 1U);

  auto doctored_proposal_b = honest_proposal_b;
  QueueEntry entry;
  entry.push_back(10);
  doctored_proposal_b.nondeterministic_bits = {uint8_t{0x03}};
  doctored_proposal_b.observed_queue_entry_sequence = {entry, entry};

  marker_branch_taken = false;
  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto ha = make_new_system_call_handler_validation(tx_insert, *insert_ctx,
                                                      insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  auto handler =
      make_new_system_call_handler_validation(tx_b, *native_ctx,
                                              std::move(doctored_proposal_b));
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  EXPECT_TRUE(marker_branch_taken);
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);

  // With the fix, block close rejects the doctored data because
  // per-TX demand (1 consume + 1 observe = 2) exceeds insert_count (1).
  reject_block(BlockExecutionMode::kBlockExecutionModeValidation);
}

// Counterexample: TX_A try_invokes a child that does 2 destructive consumes,
// then the child errors (frame reverts).  The cursor resets but observations
// at indices 0 and 1 are retained → observe_count=2.  TX_B does an honest
// consume+peek → consume_count=1, obs_count=1.  With 2 inserts this is
// valid: the consumed entry and the peeked entry are covered, and TX_A's
// observations are non-destructive.  A naive "consume_count + observe_count"
// sum check (1+2=3 > 2) would reject this incorrectly.
TEST_F(RcqDoctoredProposalDataTest,
       RevertedDoubleConsumePlusHonestConsumePeekIsValid)
{
  auto insert_ctx = make_rcq_insert_context();

  // Two separate insert TXs at kAddrB, each inserting {10}.
  auto tx_insert_1 = make_insert_tx({10});
  auto tx_insert_2 = make_insert_tx({10});
  NondeterministicProposalData insert_p1, insert_p2;
  {
    auto h1 = make_new_system_call_handler(tx_insert_1, *insert_ctx);
    ASSERT_TRUE(h1.root_invoke()().is_root_success());
    insert_p1 = h1.get_proposal_data();
    ASSERT_EQ(h1.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto h2 = make_new_system_call_handler(tx_insert_2, *insert_ctx);
    ASSERT_TRUE(h2.root_invoke()().is_root_success());
    insert_p2 = h2.get_proposal_data();
    ASSERT_EQ(h2.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // B method 0: two destructive consumes from "rcq0", then deterministic
  // error to force the try_invoke child frame to revert.
  auto b_double_consume_fault = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto r1 = ctx.handler().retrocausal_queue_consume(key, false)();
        if (!r1.has_value())
          co_return r1.error().to_invoke_error();
        auto r2 = ctx.handler().retrocausal_queue_consume(key, false)();
        if (!r2.has_value())
          co_return r2.error().to_invoke_error();
        co_return InvokeError{InvokeErrorCode::Deterministic};
      });

  // A method 0: try_invoke B.0 (which reverts after 2 consumes).
  auto a_try_invoke = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto r = try_invoke_b(ctx, 0);
        if (!r.has_value())
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        co_return InvokeStatus<void>{};
      });

  auto tx_a_ctx = make_ab_context(std::move(a_try_invoke),
                                  std::move(b_double_consume_fault));

  auto tx_a = make_invoke_a_tx(0);
  NondeterministicProposalData proposal_a;
  {
    auto ha = make_new_system_call_handler(tx_a, *tx_a_ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    proposal_a = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);

    // 2 consume bits (both found entries), child errors deterministically
    // so no try_invoke bit.  2 observations at indices 0 and 1.
    ASSERT_EQ(proposal_a.nondeterministic_bit_count, 2U);
    ASSERT_EQ(proposal_a.observed_queue_entry_sequence.size(), 2U);
  }

  // TX_B: honest consume + peek at kAddrB.
  auto b_consume_peek = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        auto r1 = ctx.handler().retrocausal_queue_consume(key, false)();
        if (!r1.has_value())
          co_return r1.error().to_invoke_error();
        auto r2 = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!r2.has_value())
          co_return r2.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cp_cmap;
  cp_cmap.emplace(1, std::move(b_consume_peek));
  auto tx_b_ctx = std::make_unique<NativeContext>(std::move(cp_cmap));

  auto tx_b = make_invoke_b_tx(0);
  NondeterministicProposalData proposal_b;
  {
    auto hb = make_new_system_call_handler(tx_b, *tx_b_ctx);
    ASSERT_TRUE(hb.root_invoke()().is_root_success());
    proposal_b = hb.get_proposal_data();
    ASSERT_EQ(hb.try_commit_transaction()(), ExecResult::ExecResultOK);

    // 2 bits (consume found, peek found).  Both the consume and peek log
    // observations to ProposalModeData; the consumed prefix is stripped at
    // delta time, not at logging time.
    ASSERT_EQ(proposal_b.nondeterministic_bit_count, 2U);
    ASSERT_EQ(proposal_b.observed_queue_entry_sequence.size(), 2U);
  }

  // Replay in validation mode with honest proposal data — must succeed.
  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto h1 = make_new_system_call_handler_validation(tx_insert_1, *insert_ctx,
                                                      insert_p1);
    ASSERT_TRUE(h1.root_invoke()().is_root_success());
    ASSERT_EQ(h1.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto h2 = make_new_system_call_handler_validation(tx_insert_2, *insert_ctx,
                                                      insert_p2);
    ASSERT_TRUE(h2.root_invoke()().is_root_success());
    ASSERT_EQ(h2.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  {
    auto ha = make_new_system_call_handler_validation(tx_a, *tx_a_ctx,
                                                      std::move(proposal_a));
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  {
    auto hb = make_new_system_call_handler_validation(tx_b, *tx_b_ctx,
                                                      std::move(proposal_b));
    ASSERT_TRUE(hb.root_invoke()().is_root_success());
    ASSERT_EQ(hb.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Block close: insert_count=2, consume_count=1, observe_count=max(2,1)=2.
  // Current independent checks both pass: 1<=2, 2<=2.
  // A naive sum check would fail: 1+2=3>2.
  ASSERT_NO_THROW(
      advance_block(BlockExecutionMode::kBlockExecutionModeValidation));
}

// ===================================================================
// Permanent-exclusive lock + RCQ observation tests
//
// A permanent-exclusive lock (ExclusiveLock flag) on a key prevents any
// other TX from acquiring a lock on that key.  These tests verify that:
//  (1) an honest proposal can't concurrently do a perm-exclusive insert
//      and an RCQ consume on the same key,
//  (2) the honest proposal replays correctly in validation, and
//  (3) a doctored proposal fabricating an RCQ observation on a
//      permanently-exclusively locked key is rejected.
// ===================================================================

// (1) In an honest proposal, a permanent-exclusive bytestring write
// on a key prevents a concurrent TX from committing an RCQ consume
// on the same key.  TX_EXCL commits a bytestring with ExclusiveLock
// on "shared".  TX_OBS peeks from "shared" (no concurrent entry) and
// also does an RCQ insert to make the child frame modify the key.
// The try_invoke child's lock acquisition fails because the key is in
// kFinalizedPermamentExclusiveLock state.
// (2) The honest proposal replays correctly in validation.
TEST_F(RcqDoctoredProposalDataTest, PermExclusiveWriteBlocksConcurrentRCQInsert)
{
  // B method 0: set_bytestring on "shared" with ExclusiveLock.
  auto b_excl_write = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("shared");
        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), key,
            std::as_bytes(std::span(std::string_view("excl"))),
            std::to_underlying(StorageDeltaFlags::ExclusiveLock))();
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> excl_cmap;
  excl_cmap.emplace(1, std::move(b_excl_write));
  auto excl_ctx = std::make_unique<NativeContext>(std::move(excl_cmap));

  // B method 0: peek from RCQ on "shared" (produces a nondeterministic
  // bit — false, no concurrent entry) then insert {10} into RCQ on
  // "shared" (modifies the key, so the child frame needs lock
  // acquisition at commit time).
  auto b_peek_and_insert = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("shared");
        auto peek = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!peek.has_value())
          co_return peek.error().to_invoke_error();
        uint8_t val = 10;
        auto ins = ctx.handler().retrocausal_queue_insert(
            key, std::as_bytes(std::span(&val, 1)))();
        if (!ins.has_value())
          co_return ins.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });

  auto a_try_peek_insert = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto r = try_invoke_b(ctx, 0);
        if (!r.has_value())
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        co_return InvokeStatus<void>{};
      });

  auto obs_ctx = make_ab_context(std::move(a_try_peek_insert),
                                 std::move(b_peek_and_insert));

  // TX_EXCL: perm-exclusive bytestring write on "shared" — commits.
  auto tx_excl = make_invoke_b_tx(0);
  NondeterministicProposalData excl_p;
  {
    auto h = make_new_system_call_handler(tx_excl, *excl_ctx);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    excl_p = h.get_proposal_data();
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // TX_OBS: A try_invokes B (peek + insert on "shared").  The peek
  // produces a nondeterministic bit (false — no concurrent entry on
  // the bytestring key).  The child frame's lock acquisition fails
  // because the key is permanently exclusively locked by TX_EXCL,
  // producing a second nondeterministic bit = false.
  auto tx_obs = make_invoke_a_tx(0);
  NondeterministicProposalData obs_p;
  {
    auto h = make_new_system_call_handler(tx_obs, *obs_ctx);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    obs_p = h.get_proposal_data();
    ASSERT_EQ(obs_p.nondeterministic_bit_count, 2U);
    ASSERT_EQ(obs_p.nondeterministic_bits[0], uint8_t{0x00})
        << "Honest proposal: peek=false (bit 0), try_invoke=false (bit 1)";
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // === Validation roundtrip with honest proposal data ===
  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto h =
        make_new_system_call_handler_validation(tx_excl, *excl_ctx, excl_p);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  {
    auto h = make_new_system_call_handler_validation(tx_obs, *obs_ctx, obs_p);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  ASSERT_NO_THROW(
      advance_block(BlockExecutionMode::kBlockExecutionModeValidation));
}

// Variant of (1): the child frame only peeks (no insert), so it has no
// modified items and the try_invoke succeeds.  The peek produces a
// nondeterministic bit (false — no concurrent entry) but no observation.
// The honest proposal roundtrips in validation.  Then we doctor the
// proposal to claim the peek found {10} — at root commit time the
// observation-only delta collides with the perm-exclusive lock.
TEST_F(RcqDoctoredProposalDataTest,
       PermExclusiveWriteBlocksDoctoredPeekOnlyObservation)
{
  auto b_excl_write = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("shared");
        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), key,
            std::as_bytes(std::span(std::string_view("excl"))),
            std::to_underlying(StorageDeltaFlags::ExclusiveLock))();
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> excl_cmap;
  excl_cmap.emplace(1, std::move(b_excl_write));
  auto excl_ctx = std::make_unique<NativeContext>(std::move(excl_cmap));

  // B method 0: peek only — no insert, so the child frame has no
  // modified items and try_invoke lock acquisition trivially succeeds.
  auto b_peek_only = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("shared");
        auto peek = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!peek.has_value())
          co_return peek.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });

  auto a_try_peek = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto r = try_invoke_b(ctx, 0);
        if (!r.has_value())
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        co_return InvokeStatus<void>{};
      });

  auto obs_ctx = make_ab_context(std::move(a_try_peek), std::move(b_peek_only));

  auto tx_excl = make_invoke_b_tx(0);
  NondeterministicProposalData excl_p;
  {
    auto h = make_new_system_call_handler(tx_excl, *excl_ctx);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    excl_p = h.get_proposal_data();
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Honest proposal: peek=false (bit 0), try_invoke=true (bit 1, no
  // modified items so lock acquisition succeeds).  No observations.
  auto tx_obs = make_invoke_a_tx(0);
  NondeterministicProposalData obs_p;
  {
    auto h = make_new_system_call_handler(tx_obs, *obs_ctx);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    obs_p = h.get_proposal_data();
    ASSERT_EQ(obs_p.nondeterministic_bit_count, 2U);
    ASSERT_EQ(obs_p.nondeterministic_bits[0], uint8_t{0x02})
        << "Honest: peek=false (bit 0), try_invoke=true (bit 1)";
    ASSERT_EQ(obs_p.observed_queue_entry_sequence.size(), 0U);
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Honest roundtrip in validation.
  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
  {
    auto h =
        make_new_system_call_handler_validation(tx_excl, *excl_ctx, excl_p);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  {
    auto h = make_new_system_call_handler_validation(tx_obs, *obs_ctx, obs_p);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  // Discard (rather than commit) the honest validation block: the doctored
  // replay below reuses the same tx_excl/tx_obs, so committing the honest block
  // would persist their replay nonces and make the doctored re-run fail as a
  // duplicate instead of for the intended (forged-observation) reason. Closing
  // it still verifies the honest case closes cleanly without throwing.
  ASSERT_NO_THROW(
      discard_block(BlockExecutionMode::kBlockExecutionModeValidation));

  // Doctor the proposal: claim peek found {10} (both bits true).
  // The delta at root commit is observation-only (no insert) on a
  // perm-exclusive key → NonDeterministicError.
  NondeterministicProposalData doctored;
  doctored.nondeterministic_bit_count = 2;
  doctored.nondeterministic_bits = {uint8_t{0x03}};
  QueueEntry entry;
  entry.push_back(10);
  doctored.observed_queue_entry_sequence = {entry};

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
  {
    auto h =
        make_new_system_call_handler_validation(tx_excl, *excl_ctx, excl_p);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  auto handler = make_new_system_call_handler_validation(tx_obs, *obs_ctx,
                                                         std::move(doctored));
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction()();
  EXPECT_EQ(commit_res, ExecResult::ExecResultTxNonDeterministicError)
      << "Observation-only delta must collide with perm-exclusive lock";
}

// (3) A doctored proposal claims that a try_invoke succeeded on a
// permanently-exclusively locked key AND that it observed a concurrent
// RCQ entry.  Validation must reject this: the perm-exclusive lock
// makes such an observation impossible.
//
// The contract B does only a peek (no self-insert) so the peek reaches
// the concurrent-entry path and actually consumes the nondeterministic
// bit + observation from the doctored proposal.  At root commit time,
// the observation delta for "shared" collides with the perm-exclusive
// lock → NonDeterministicError.
TEST_F(RcqDoctoredProposalDataTest,
       DoctoredRCQObservationOnPermExclusiveKeyRejected)
{
  // B method 0: set_bytestring on "shared" with ExclusiveLock.
  auto b_excl_write = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("shared");
        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), key,
            std::as_bytes(std::span(std::string_view("excl"))),
            std::to_underlying(StorageDeltaFlags::ExclusiveLock))();
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> excl_cmap;
  excl_cmap.emplace(1, std::move(b_excl_write));
  auto excl_ctx = std::make_unique<NativeContext>(std::move(excl_cmap));

  // B method 0: peek from RCQ on "shared" (no self-insert, so the peek
  // goes through the concurrent-entry path and consumes a nondeterministic
  // bit + observation).
  auto b_peek = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("shared");
        auto peek = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!peek.has_value())
          co_return peek.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });

  auto a_try_peek = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto r = try_invoke_b(ctx, 0);
        if (!r.has_value())
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        co_return InvokeStatus<void>{};
      });

  auto obs_ctx = make_ab_context(std::move(a_try_peek), std::move(b_peek));

  // TX_EXCL: perm-exclusive bytestring write on "shared".
  auto tx_excl = make_invoke_b_tx(0);
  NondeterministicProposalData excl_p;
  {
    auto h = make_new_system_call_handler(tx_excl, *excl_ctx);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    excl_p = h.get_proposal_data();
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // TX_OBS: A try_invokes B (peek on "shared").  The peek produces a
  // nondeterministic bit (false — no concurrent entry on the bytestring
  // key) and the try_invoke child succeeds (no modified items to lock).
  // 2 bits total: [peek=false, try_invoke=true].
  auto tx_obs = make_invoke_a_tx(0);
  NondeterministicProposalData honest_p;
  {
    auto h = make_new_system_call_handler(tx_obs, *obs_ctx);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    honest_p = h.get_proposal_data();
    ASSERT_EQ(honest_p.nondeterministic_bit_count, 2U);
    ASSERT_EQ(honest_p.nondeterministic_bits[0], uint8_t{0x02})
        << "Honest bits: peek=false (bit 0), try_invoke=true (bit 1)";
    ASSERT_EQ(honest_p.observed_queue_entry_sequence.size(), 0U);
    ASSERT_TRUE(h.abort_transaction()().has_value());
  }

  // Doctor the proposal: claim peek found {10} AND try_invoke succeeded.
  NondeterministicProposalData doctored;
  doctored.nondeterministic_bit_count = 2;
  doctored.nondeterministic_bits = {uint8_t{0x03}};
  QueueEntry entry;
  entry.push_back(10);
  doctored.observed_queue_entry_sequence = {entry};

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto h =
        make_new_system_call_handler_validation(tx_excl, *excl_ctx, excl_p);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Replay TX_OBS with doctored data.  Both nondeterministic bits are
  // consumed (peek=true, try_invoke=true).  Validation skips lock
  // acquisition for the child frame.  At root commit time,
  // try_acquire_frame_locks submits an observation-only delta for "shared"
  // which collides with the perm-exclusive lock → NonDeterministicError.
  auto handler = make_new_system_call_handler_validation(tx_obs, *obs_ctx,
                                                         std::move(doctored));
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction()();
  EXPECT_EQ(commit_res, ExecResult::ExecResultTxNonDeterministicError)
      << "Observation delta must collide with perm-exclusive lock";
}

// Variant: the doctored proposal claims a root-frame RCQ observation
// (no try_invoke) on a permanently-exclusively locked key.  The root
// frame manually reverts, exercising the PR #2365 manual-revert
// observation submission path.  The observation delta must collide
// with the perm-exclusive lock and the block must be rejected.
TEST_F(RcqDoctoredProposalDataTest,
       DoctoredRootManualRevertRCQObservationOnPermExclusiveKey)
{
  // TX_EXCL: B method 0 does set_bytestring with ExclusiveLock on "shared".
  auto b_excl_write = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("shared");
        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), key,
            std::as_bytes(std::span(std::string_view("excl"))),
            std::to_underlying(StorageDeltaFlags::ExclusiveLock))();
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> excl_cmap;
  excl_cmap.emplace(1, std::move(b_excl_write));
  auto excl_ctx = std::make_unique<NativeContext>(std::move(excl_cmap));

  // TX_OBS: B method 0 peeks RCQ on "shared".  If it sees an entry,
  // it triggers manual root-frame revert.  Otherwise it writes a marker.
  static constexpr uint8_t kForgedVal = 0xAB;
  auto b_peek_and_maybe_revert = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("shared");
        auto peek = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!peek.has_value())
          co_return peek.error().to_invoke_error();

        if (peek.value().has_value() && peek.value().value().size() == 1 &&
            peek.value().value()[0] == kForgedVal) {
          ctx.handler().set_return_buffer(ReturnBuffer{}, true);
          co_return InvokeStatus<void>{};
        }

        auto marker_key = make_storage_key("marker");
        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), marker_key,
            std::as_bytes(std::span(std::string_view("honest"))),
            std::to_underlying(StorageDeltaFlags::None))();
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> obs_cmap;
  obs_cmap.emplace(1, std::move(b_peek_and_maybe_revert));
  auto obs_ctx = std::make_unique<NativeContext>(std::move(obs_cmap));

  // TX_EXCL commits perm-exclusive lock on "shared".
  auto tx_excl = make_invoke_b_tx(0);
  NondeterministicProposalData excl_p;
  {
    auto h = make_new_system_call_handler(tx_excl, *excl_ctx);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    excl_p = h.get_proposal_data();
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // TX_OBS honest proposal: peek finds no concurrent entry (bit=false).
  auto tx_obs = make_invoke_b_tx(0);
  {
    auto h = make_new_system_call_handler(tx_obs, *obs_ctx);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    auto honest_p = h.get_proposal_data();
    ASSERT_EQ(honest_p.nondeterministic_bit_count, 1U);
    ASSERT_EQ(honest_p.nondeterministic_bits[0] & uint8_t{1}, 0U);
    ASSERT_EQ(honest_p.observed_queue_entry_sequence.size(), 0U);
    ASSERT_TRUE(h.abort_transaction()().has_value());
  }

  // Doctor the proposal: claim peek found {kForgedVal}, triggering revert.
  NondeterministicProposalData forged;
  forged.nondeterministic_bit_count = 1;
  forged.nondeterministic_bits = {uint8_t{1}};
  forged.observed_queue_entry_sequence = {QueueEntry{kForgedVal}};

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto h =
        make_new_system_call_handler_validation(tx_excl, *excl_ctx, excl_p);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Replay TX_OBS with forged data.  The peek consumes the doctored
  // observation and the contract chooses manual revert.  The PR #2365
  // manual-revert path tries to submit the observation delta to HogDb,
  // but the key is perm-exclusively locked → NonDeterministicError.
  auto handler = make_new_system_call_handler_validation(tx_obs, *obs_ctx,
                                                         std::move(forged));
  auto obs_res = handler.root_invoke()();
  ASSERT_TRUE(obs_res.is_root_success())
      << "The forged observation is consumed and triggers manual revert";
  auto commit_res = handler.try_commit_transaction()();
  EXPECT_EQ(commit_res, ExecResult::ExecResultTxNonDeterministicError)
      << "Observation delta must collide with perm-exclusive lock";
}

// ===================================================================
// TODO(geoff): Permanent-exclusive RCQ insert + concurrent consume via syscall
//
// The retrocausal_queue_insert syscall does not currently accept a
// StorageDeltaFlags parameter, so there is no way to produce a
// permanent-exclusive RCQ insert through the normal execution path.
// If/when we add ExclusiveLock support to retrocausal_queue_insert,
// these two test cases should be rewritten as full syscall-level
// propose/validate roundtrips (like the tests above):
//
// (A) PermExclusiveRCQInsertBlocksConcurrentConsume
//   TX_A inserts {10} into RCQ on key K with ExclusiveLock, commits.
//   TX_B tries to consume from key K — should fail in proposal mode
//   because the key is in kFinalizedPermamentExclusiveLock state.
//   Then replay both TXs in validation with honest proposal data
//   (TX_B's bit = false).  Doctor TX_B's proposal to claim bit = true
//   with observation {10} — validation must reject: either the root
//   commit's observation delta collides with the perm-exclusive lock
//   (NonDeterministicError), or block close detects no matching insert
//   in entry_tracking_ (the insert was committed under exclusive lock,
//   so the observation delta never reaches RetrocausalQueueLock).
//
// (B) PermExclusiveRCQInsertBlocksRevertedConsume
//   Same as (A), but TX_B's consume is inside a try_invoke child
//   frame that reverts (so we only get the observation, no destructive
//   consume).  The reverted frame's observations are still tracked in
//   ExecutionModeData.  Doctor the proposal to claim the observation
//   existed — at root commit time, the observation-only delta for key K
//   must collide with the perm-exclusive lock.  This exercises the same
//   path as DoctoredRootManualRevertRCQObservationOnPermExclusiveKey
//   above but with an actual RCQ insert (not a bytestring) holding the
//   exclusive lock, which would cover entry_tracking_ accounting too.
//
// The direct-delta versions below exercise the same invariants at the
// database layer by constructing StorageDeltas manually and submitting
// them through PerTransactionDatabaseProxy.
// ===================================================================

// Direct-delta test: TX_A inserts {10} into RCQ with ExclusiveLock.
// TX_B cannot concurrently consume from the same key (lock conflict).
// TX_B also cannot sneak in an observation-only delta claiming to have
// seen the insert — the perm-exclusive lock blocks the push.
TEST_F(RcqDoctoredProposalDataTest,
       DirectDeltaPermExclusiveRCQInsertBlocksConcurrentConsume)
{
  setup_grant_storage_token(kAddrB, 1'000'000'000);

  auto addrkey = make_contract_data_addrkey(kAddrB, "rcq0");
  QueueEntry entry;
  entry.push_back(10);

  // TX_A: RCQ insert {10} with ExclusiveLock — commits.
  auto tx_a = make_new_per_tx_db_proxy(UINT64_MAX);
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().insert =
        RetrocausalQueueInsertDelta{.added_entries = {entry}};
    tx_a.add_delta(addrkey, std::move(delta),
                   std::to_underlying(StorageDeltaFlags::ExclusiveLock));
    auto r = tx_a.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value() && r.value());
    ASSERT_TRUE(tx_a.commit_transaction()().has_value());
  }

  // TX_B: try to consume {10} from the same key — must fail because
  // TX_A holds a permanent exclusive lock.
  auto tx_b = make_new_per_tx_db_proxy(UINT64_MAX);
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().consume = RetrocausalQueueConsumeDelta{
        .past_block_consume = {.max_fully_consumed_block_number = 0,
                               .partial_consumed_number = 0},
        .current_block_consumed = {entry}};
    tx_b.add_delta(addrkey, std::move(delta),
                   std::to_underlying(StorageDeltaFlags::None));
    auto r = tx_b.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r.value())
        << "Consume must fail: key is permanently exclusively locked";
    ASSERT_TRUE(tx_b.abort_transaction()().has_value());
  }

  // TX_C: try to push an observation-only delta (what a doctored
  // validator would attempt) — must also fail.
  auto tx_c = make_new_per_tx_db_proxy(UINT64_MAX);
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().observed_concurrent_entries = {entry};
    tx_c.add_delta(addrkey, std::move(delta),
                   std::to_underlying(StorageDeltaFlags::None));
    auto r = tx_c.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r.value())
        << "Observation-only delta must fail: key is permanently exclusively "
           "locked";
    ASSERT_TRUE(tx_c.abort_transaction()().has_value());
  }

  advance_block();
}

// Direct-delta test: same scenario but TX_B only has an observation
// (the consume was in a reverted frame, so only the observation-only
// delta remains).  The perm-exclusive lock must still block it.
// Then verify that even if somehow the observation reached
// entry_tracking_, block close would reject it (insert_count comes
// from commit_change which only runs under the exclusive lock holder).
TEST_F(RcqDoctoredProposalDataTest,
       DirectDeltaPermExclusiveRCQInsertBlocksObservationOnly)
{
  setup_grant_storage_token(kAddrB, 1'000'000'000);

  auto addrkey = make_contract_data_addrkey(kAddrB, "rcq0");
  QueueEntry entry;
  entry.push_back(10);

  // TX_A: RCQ insert {10} with ExclusiveLock — commits.
  auto tx_a = make_new_per_tx_db_proxy(UINT64_MAX);
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().insert =
        RetrocausalQueueInsertDelta{.added_entries = {entry}};
    tx_a.add_delta(addrkey, std::move(delta),
                   std::to_underlying(StorageDeltaFlags::ExclusiveLock));
    auto r = tx_a.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value() && r.value());
    ASSERT_TRUE(tx_a.commit_transaction()().has_value());
  }

  // TX_B: observation-only delta claiming to have seen {10} from the
  // concurrent entries.  This is what remains after a reverted consume
  // frame — the destructive consume is gone but the observation persists.
  auto tx_b = make_new_per_tx_db_proxy(UINT64_MAX);
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().observed_concurrent_entries = {entry};
    tx_b.add_delta(addrkey, std::move(delta),
                   std::to_underlying(StorageDeltaFlags::None));
    auto r = tx_b.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r.value())
        << "Observation-only delta on a perm-exclusive key must be rejected "
           "at the lock level";
    ASSERT_TRUE(tx_b.abort_transaction()().has_value());
  }

  advance_block();
}

// Direct-delta test: reversed commit order — TX_B's observation-only
// delta commits first (as could happen in validation mode where TX
// ordering is dictated by the proposer), then TX_A's exclusive RCQ
// insert comes after.  The exclusive insert must fail to acquire the
// lock because handle_try_atomic_from_finalized unconditionally rejects
// exclusive locks when the key is already in kFinalized state (TX_B
// holds a non-exclusive ref).  TX_B's orphaned observation then causes
// block close to reject (no matching insert in entry_tracking_).
TEST_F(RcqDoctoredProposalDataTest,
       DirectDeltaObservationBeforePermExclusiveRCQInsert)
{
  setup_grant_storage_token(kAddrB, 1'000'000'000);

  auto addrkey = make_contract_data_addrkey(kAddrB, "rcq0");
  QueueEntry entry;
  entry.push_back(10);

  // TX_B commits first: observation-only delta claiming {10} exists
  // as a concurrent entry.
  auto tx_b = make_new_per_tx_db_proxy(UINT64_MAX);
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().observed_concurrent_entries = {entry};
    tx_b.add_delta(addrkey, std::move(delta),
                   std::to_underlying(StorageDeltaFlags::None));
    auto r = tx_b.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value() && r.value());
    ASSERT_TRUE(tx_b.commit_transaction()().has_value());
  }

  // TX_A: RCQ insert {10} with ExclusiveLock — must fail because
  // TX_B already holds a non-exclusive ref on the key.
  auto tx_a = make_new_per_tx_db_proxy(UINT64_MAX);
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().insert =
        RetrocausalQueueInsertDelta{.added_entries = {entry}};
    tx_a.add_delta(addrkey, std::move(delta),
                   std::to_underlying(StorageDeltaFlags::ExclusiveLock));
    auto r = tx_a.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r.value())
        << "Exclusive lock must fail: TX_B already holds a non-exclusive ref";
    ASSERT_TRUE(tx_a.abort_transaction()().has_value());
  }

  // TX_B's observation is committed with no matching insert —
  // block close rejects.
  reject_block(BlockExecutionMode::kBlockExecutionModeProposal);
}

// ===================================================================
// Additional coverage: destructive consume, root-frame consume,
// multiple observers, and prior-block entries.
// ===================================================================

// Doctored destructive consume (not peek) on a perm-exclusive key.
// The consume produces a RetrocausalQueueConsumeDelta with
// current_block_consumed entries — a different delta shape from the
// observation-only deltas tested above.  The perm-exclusive lock must
// still block it at the ItemLock level.
TEST_F(RcqDoctoredProposalDataTest,
       DirectDeltaPermExclusiveBlocksDestructiveConsume)
{
  setup_grant_storage_token(kAddrB, 1'000'000'000);

  auto addrkey = make_contract_data_addrkey(kAddrB, "rcq0");
  QueueEntry entry;
  entry.push_back(10);

  // TX_A: RCQ insert {10} with ExclusiveLock — commits.
  auto tx_a = make_new_per_tx_db_proxy(UINT64_MAX);
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().insert =
        RetrocausalQueueInsertDelta{.added_entries = {entry}};
    tx_a.add_delta(addrkey, std::move(delta),
                   std::to_underlying(StorageDeltaFlags::ExclusiveLock));
    auto r = tx_a.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value() && r.value());
    ASSERT_TRUE(tx_a.commit_transaction()().has_value());
  }

  // TX_B: destructive consume of {10} + observation.  This is the delta
  // shape produced by a non-peek retrocausal_queue_consume that finds a
  // concurrent entry.
  auto tx_b = make_new_per_tx_db_proxy(UINT64_MAX);
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().consume = RetrocausalQueueConsumeDelta{
        .past_block_consume = {.max_fully_consumed_block_number = 0,
                               .partial_consumed_number = 0},
        .current_block_consumed = {entry}};
    delta.retrocausal_queue().observed_concurrent_entries = {entry};
    tx_b.add_delta(addrkey, std::move(delta),
                   std::to_underlying(StorageDeltaFlags::None));
    auto r = tx_b.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r.value())
        << "Destructive consume + observation must fail on perm-exclusive key";
    ASSERT_TRUE(tx_b.abort_transaction()().has_value());
  }

  advance_block();
}

// Root-frame destructive consume on a perm-exclusive key (no try_invoke
// wrapper).  The root frame's try_acquire_frame_locks submits the
// consume + observation delta directly — there is no nondeterministic
// bit fallback.  The delta must be rejected at the lock level,
// producing NonDeterministicError at root commit time.
TEST_F(RcqDoctoredProposalDataTest,
       DoctoredRootFrameDestructiveConsumeOnPermExclusiveKey)
{
  // B method 0: set_bytestring on "shared" with ExclusiveLock.
  auto b_excl_write = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("shared");
        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), key,
            std::as_bytes(std::span(std::string_view("excl"))),
            std::to_underlying(StorageDeltaFlags::ExclusiveLock))();
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> excl_cmap;
  excl_cmap.emplace(1, std::move(b_excl_write));
  auto excl_ctx = std::make_unique<NativeContext>(std::move(excl_cmap));

  // B method 0: destructive consume (peek=false) on "shared".
  auto b_consume = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("shared");
        auto res = ctx.handler().retrocausal_queue_consume(key, false)();
        if (!res.has_value())
          co_return res.error().to_invoke_error();
        co_return InvokeStatus<void>{};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cons_cmap;
  cons_cmap.emplace(1, std::move(b_consume));
  auto cons_ctx = std::make_unique<NativeContext>(std::move(cons_cmap));

  auto tx_excl = make_invoke_b_tx(0);
  NondeterministicProposalData excl_p;
  {
    auto h = make_new_system_call_handler(tx_excl, *excl_ctx);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    excl_p = h.get_proposal_data();
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Honest proposal: destructive consume finds nothing (bit=false).
  auto tx_cons = make_invoke_b_tx(0);
  {
    auto h = make_new_system_call_handler(tx_cons, *cons_ctx);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    auto honest_p = h.get_proposal_data();
    ASSERT_EQ(honest_p.nondeterministic_bit_count, 1U);
    ASSERT_EQ(honest_p.nondeterministic_bits[0] & uint8_t{1}, 0U);
    ASSERT_EQ(honest_p.observed_queue_entry_sequence.size(), 0U);
    ASSERT_TRUE(h.abort_transaction()().has_value());
  }

  // Doctor: claim consume found {10}.
  NondeterministicProposalData doctored;
  doctored.nondeterministic_bit_count = 1;
  doctored.nondeterministic_bits = {uint8_t{1}};
  QueueEntry entry;
  entry.push_back(10);
  doctored.observed_queue_entry_sequence = {entry};

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto h =
        make_new_system_call_handler_validation(tx_excl, *excl_ctx, excl_p);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Root-frame consume with doctored data.  The consume persists into
  // the ItemView (advance_cursor=true), so try_acquire_frame_locks
  // generates a delta with both consume and observation fields.
  // The perm-exclusive lock blocks the push → NonDeterministicError.
  auto handler = make_new_system_call_handler_validation(tx_cons, *cons_ctx,
                                                         std::move(doctored));
  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  auto commit_res = handler.try_commit_transaction()();
  EXPECT_EQ(commit_res, ExecResult::ExecResultTxNonDeterministicError)
      << "Root-frame consume delta must collide with perm-exclusive lock";
}

// Multiple observers on the same perm-exclusive key.  Verifies that
// kFinalizedPermamentExclusiveLock is stable under multiple independent
// rejection attempts — each TX gets kConflict independently.
TEST_F(RcqDoctoredProposalDataTest,
       DirectDeltaMultipleObserversOnPermExclusiveKey)
{
  setup_grant_storage_token(kAddrB, 1'000'000'000);

  auto addrkey = make_contract_data_addrkey(kAddrB, "rcq0");
  QueueEntry entry_a, entry_b, entry_c;
  entry_a.push_back(10);
  entry_b.push_back(20);
  entry_c.push_back(30);

  // TX_EXCL: RCQ insert with ExclusiveLock — commits.
  auto tx_excl = make_new_per_tx_db_proxy(UINT64_MAX);
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().insert =
        RetrocausalQueueInsertDelta{.added_entries = {entry_a}};
    tx_excl.add_delta(addrkey, std::move(delta),
                      std::to_underlying(StorageDeltaFlags::ExclusiveLock));
    auto r = tx_excl.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value() && r.value());
    ASSERT_TRUE(tx_excl.commit_transaction()().has_value());
  }

  // Three independent observers — all must fail.
  for (auto const &obs_entry : {entry_a, entry_b, entry_c}) {
    auto tx = make_new_per_tx_db_proxy(UINT64_MAX);
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().observed_concurrent_entries = {obs_entry};
    tx.add_delta(addrkey, std::move(delta),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r.value()) << "Observer must fail on perm-exclusive key";
    ASSERT_TRUE(tx.abort_transaction()().has_value());
  }

  advance_block();
}

// Prior-block RCQ entries + current-block perm-exclusive write.
// Block 1 inserts entries into the RCQ (persisted to DAO).  Block 2
// has a perm-exclusive write on the same key.  A concurrent TX trying
// to consume the prior-block entries must be blocked by the
// perm-exclusive lock — the past_block_consume DAO path in
// RetrocausalQueueLock::try_atomic_change_lock is never reached.
TEST_F(RcqDoctoredProposalDataTest,
       DirectDeltaPriorBlockEntriesWithPermExclusiveWrite)
{
  setup_grant_storage_token(kAddrB, 1'000'000'000);

  auto addrkey = make_contract_data_addrkey(kAddrB, "rcq0");
  QueueEntry entry;
  entry.push_back(10);

  // Block 1: insert {10} into RCQ (no exclusive lock, normal insert).
  {
    auto tx = make_new_per_tx_db_proxy(UINT64_MAX);
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().insert =
        RetrocausalQueueInsertDelta{.added_entries = {entry}};
    tx.add_delta(addrkey, std::move(delta),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value() && r.value());
    ASSERT_TRUE(tx.commit_transaction()().has_value());
  }
  advance_block();

  // Block 2: TX_EXCL does a perm-exclusive bytestring write on the
  // same key (type change from RCQ to bytestring).
  {
    auto tx = make_new_per_tx_db_proxy(UINT64_MAX);
    tx.add_delta(addrkey, make_bytestring_delta("excl"),
                 std::to_underlying(StorageDeltaFlags::ExclusiveLock));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value() && r.value());
    ASSERT_TRUE(tx.commit_transaction()().has_value());
  }

  // TX_B: try to consume the prior-block entry via past_block_consume.
  // Must fail at the ItemLock level (perm-exclusive) before reaching
  // the DAO read path in RetrocausalQueueLock::try_atomic_change_lock.
  {
    auto tx = make_new_per_tx_db_proxy(UINT64_MAX);
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().consume = RetrocausalQueueConsumeDelta{
        .past_block_consume = {.max_fully_consumed_block_number = 1,
                               .partial_consumed_number = 1},
        .current_block_consumed = {}};
    tx.add_delta(addrkey, std::move(delta),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r.value())
        << "Prior-block consume must fail: key is perm-exclusively locked";
    ASSERT_TRUE(tx.abort_transaction()().has_value());
  }

  // TX_C: observation-only on the same key — also blocked.
  {
    auto tx = make_new_per_tx_db_proxy(UINT64_MAX);
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().observed_concurrent_entries = {entry};
    tx.add_delta(addrkey, std::move(delta),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r.value())
        << "Observation on prior-block key must fail: perm-exclusively locked";
    ASSERT_TRUE(tx.abort_transaction()().has_value());
  }

  advance_block();
}

// Doctored validation data supplying kRetrocausalQueueBlockEntriesMaxLen + 1
// concurrent observations for one key. Consume #250 is accepted; consume #251
// trips the per-tx concurrent-consume cap in
// RetrocausalQueueItem::get_next_item and must surface as ValidationFailure
// (in proposal mode the same arm is a CHECK: HogDb's commit-time reservation
// cap keeps committed inserts per (block, key) at or below the cap, so a
// proposal-mode consumer can never see a 251st concurrent entry).
TEST_F(RcqDoctoredProposalDataTest,
       ConcurrentConsumeCapExceededIsValidationFailure)
{
  constexpr uint32_t kOverCap = kRetrocausalQueueBlockEntriesMaxLen + 1;

  uint32_t consumes_succeeded = 0;
  auto consumer_contract =
      build_contract([&consumes_succeeded](NativeSyscallContext &ctx) noexcept
                         -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("rcq0");
        for (uint32_t i = 0; i < kOverCap; ++i) {
          auto res = ctx.handler().retrocausal_queue_consume(key, false)();
          if (!res.has_value())
            co_return res.error().to_invoke_error();
          if (!res.value().has_value())
            co_return InvokeError{InvokeErrorCode::Deterministic};
          ++consumes_succeeded;
        }
        co_return InvokeStatus<void>{};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(1, std::move(consumer_contract));
  auto native_ctx = std::make_unique<NativeContext>(std::move(cmap));

  NondeterministicProposalData doctored;
  doctored.nondeterministic_bit_count = kOverCap;
  doctored.nondeterministic_bits.resize((kOverCap + 7) / 8, 0);
  for (uint32_t i = 0; i < kOverCap; ++i) {
    doctored.nondeterministic_bits[i / 8] |=
        static_cast<uint8_t>(uint8_t{1} << (i % 8));
  }
  QueueEntry entry;
  entry.push_back(7);
  for (uint32_t i = 0; i < kOverCap; ++i) {
    doctored.observed_queue_entry_sequence.push_back(entry);
  }

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  auto tx = make_invoke_b_tx(0);
  auto handler = make_new_system_call_handler_validation(tx, *native_ctx,
                                                         std::move(doctored));
  auto res = handler.root_invoke()();
  ASSERT_FALSE(res.is_root_success());
  EXPECT_EQ(res.result_.error().code(), InvokeErrorCode::ValidationFailure);
  EXPECT_EQ(consumes_succeeded, kRetrocausalQueueBlockEntriesMaxLen);
  EXPECT_EQ(handler.try_commit_transaction()(),
            ExecResult::ExecResultTxValidationFailure);
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
