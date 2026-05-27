#include <gtest/gtest.h>

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
          TestDB::kHogDB)
  {}

protected:
  void SetUp() override
  {
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
    RuntimeMemoryLength output_return_buffer_len = 0,
                        output_returned_auth_token_len = 0;
    return ctx.syscall<SyscallNo::kInvoke>(
        kAddrB, method, UINT64_MAX, Calldata{},
        std::to_underlying(InvokeFlag::TryInvoke), std::span<uint32_t>{},
        output_return_buffer_len, output_returned_auth_token_len);
  }

  static std::unique_ptr<RuntimeContext> make_rcq_insert_context()
  {
    auto contract = build_contract(
        [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          auto const &cd = ctx.handler().get_calldata();
          auto key = make_storage_key("rcq0");
          for (uint8_t val : cd) {
            auto res = ctx.handler().retrocausal_queue_insert(
                key, std::as_bytes(std::span(&val, 1)));
            if (!res.has_value())
              return res.error().to_invoke_error();
          }
          return {};
        });
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
    cmap.emplace(1, std::move(contract));
    return std::make_unique<NativeContext>(std::move(cmap));
  }

  static Transaction make_insert_tx(std::vector<uint8_t> const &values)
  {
    return Transaction{
        .invocation =
            Invocation{.invoked_address = kAddrB,
                       .entrypoint = entrypoint_from_method_id(0),
                       .gas_limit = UINT64_MAX,
                       .calldata = Calldata(values.begin(), values.end())}};
  }

  static Transaction make_invoke_a_tx(Runtime::MethodID method)
  {
    return Transaction{
        .invocation =
            Invocation{.invoked_address = kAddrA,
                       .entrypoint = entrypoint_from_method_id(method),
                       .gas_limit = UINT64_MAX,
                       .calldata = {}}};
  }

  static Transaction make_invoke_b_tx(Runtime::MethodID method)
  {
    return Transaction{
        .invocation =
            Invocation{.invoked_address = kAddrB,
                       .entrypoint = entrypoint_from_method_id(method),
                       .gas_limit = UINT64_MAX,
                       .calldata = {}}};
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
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  bool marker_branch_taken = false;
  auto consume_then_peek_marker_contract = build_contract(
      [&marker_branch_taken](
          NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto key = make_storage_key("rcq0");
        QueueEntry expected_entry;
        expected_entry.push_back(10);

        auto consume_res = ctx.handler().retrocausal_queue_consume(key, false);
        if (!consume_res.has_value())
          return consume_res.error().to_invoke_error();

        auto peek_res = ctx.handler().retrocausal_queue_consume(key, true);
        if (!peek_res.has_value())
          return peek_res.error().to_invoke_error();

        if (!consume_res.value().has_value() || !peek_res.value().has_value() ||
            consume_res.value().value() != expected_entry ||
            peek_res.value().value() != expected_entry) {
          return {};
        }

        marker_branch_taken = true;
        auto marker_key = make_storage_key("marker");
        uint8_t marker[] = {0xEE};
        auto ws = ctx.handler().set_bytestring(
            marker_key, std::as_bytes(std::span(marker)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!ws.has_value())
          return ws.error().to_invoke_error();
        if (!ws.value().has_value())
          return InvokeError{InvokeErrorCode::Deterministic};
        return {};
      });

  // The PoC contract runs at kAddrB (same address as the RCQ inserts).
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(1, std::move(consume_then_peek_marker_contract));
  auto native_ctx = std::make_unique<NativeContext>(std::move(cmap));

  auto tx_b = make_invoke_b_tx(0);
  auto honest_proposal_b = [&] {
    auto ha = make_new_system_call_handler(tx_b, *native_ctx);
    EXPECT_TRUE(ha.root_invoke().is_root_success());
    auto p = ha.get_proposal_data();
    EXPECT_TRUE(ha.abort_transaction().has_value());
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
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  auto handler = make_new_system_call_handler_validation(
      tx_b, *native_ctx, std::move(doctored_proposal_b));
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  EXPECT_TRUE(marker_branch_taken);
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);

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
    ASSERT_TRUE(h1.root_invoke().is_root_success());
    insert_p1 = h1.get_proposal_data();
    ASSERT_EQ(h1.try_commit_transaction(), ExecResult::ExecResultOK);

    auto h2 = make_new_system_call_handler(tx_insert_2, *insert_ctx);
    ASSERT_TRUE(h2.root_invoke().is_root_success());
    insert_p2 = h2.get_proposal_data();
    ASSERT_EQ(h2.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  // B method 0: two destructive consumes from "rcq0", then deterministic
  // error to force the try_invoke child frame to revert.
  auto b_double_consume_fault = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto key = make_storage_key("rcq0");
        auto r1 = ctx.handler().retrocausal_queue_consume(key, false);
        if (!r1.has_value())
          return r1.error().to_invoke_error();
        auto r2 = ctx.handler().retrocausal_queue_consume(key, false);
        if (!r2.has_value())
          return r2.error().to_invoke_error();
        return InvokeError{InvokeErrorCode::Deterministic};
      });

  // A method 0: try_invoke B.0 (which reverts after 2 consumes).
  auto a_try_invoke = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto r = try_invoke_b(ctx, 0);
        if (!r.has_value())
          return InvokeError{InvokeErrorCode::Unrecoverable};
        return {};
      });

  auto tx_a_ctx =
      make_ab_context(std::move(a_try_invoke), std::move(b_double_consume_fault));

  auto tx_a = make_invoke_a_tx(0);
  NondeterministicProposalData proposal_a;
  {
    auto ha = make_new_system_call_handler(tx_a, *tx_a_ctx);
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    proposal_a = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);

    // 2 consume bits (both found entries), child errors deterministically
    // so no try_invoke bit.  2 observations at indices 0 and 1.
    ASSERT_EQ(proposal_a.nondeterministic_bit_count, 2U);
    ASSERT_EQ(proposal_a.observed_queue_entry_sequence.size(), 2U);
  }

  // TX_B: honest consume + peek at kAddrB.
  auto b_consume_peek = build_contract(
      [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto key = make_storage_key("rcq0");
        auto r1 = ctx.handler().retrocausal_queue_consume(key, false);
        if (!r1.has_value())
          return r1.error().to_invoke_error();
        auto r2 = ctx.handler().retrocausal_queue_consume(key, true);
        if (!r2.has_value())
          return r2.error().to_invoke_error();
        return {};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cp_cmap;
  cp_cmap.emplace(1, std::move(b_consume_peek));
  auto tx_b_ctx = std::make_unique<NativeContext>(std::move(cp_cmap));

  auto tx_b = make_invoke_b_tx(0);
  NondeterministicProposalData proposal_b;
  {
    auto hb = make_new_system_call_handler(tx_b, *tx_b_ctx);
    ASSERT_TRUE(hb.root_invoke().is_root_success());
    proposal_b = hb.get_proposal_data();
    ASSERT_EQ(hb.try_commit_transaction(), ExecResult::ExecResultOK);

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
    ASSERT_TRUE(h1.root_invoke().is_root_success());
    ASSERT_EQ(h1.try_commit_transaction(), ExecResult::ExecResultOK);

    auto h2 = make_new_system_call_handler_validation(tx_insert_2, *insert_ctx,
                                                       insert_p2);
    ASSERT_TRUE(h2.root_invoke().is_root_success());
    ASSERT_EQ(h2.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  {
    auto ha = make_new_system_call_handler_validation(tx_a, *tx_a_ctx,
                                                      std::move(proposal_a));
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);
  }
  {
    auto hb = make_new_system_call_handler_validation(tx_b, *tx_b_ctx,
                                                      std::move(proposal_b));
    ASSERT_TRUE(hb.root_invoke().is_root_success());
    ASSERT_EQ(hb.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  // Block close: insert_count=2, consume_count=1, observe_count=max(2,1)=2.
  // Current independent checks both pass: 1<=2, 2<=2.
  // A naive sum check would fail: 1+2=3>2.
  ASSERT_NO_THROW(
      advance_block(BlockExecutionMode::kBlockExecutionModeValidation));
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
