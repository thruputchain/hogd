// Tolerated proposal/validation anomaly: validators trust the proposer-supplied
// TryInvoke lock-admission bit instead of re-running the per-child HogDB
// admission check.
//
// Background.  A TryInvoke child frame runs a two-phase-commit "early lock
// acquisition" at the child boundary.  In proposal mode finish_frame() calls
// InvocationFrame::try_acquire_frame_locks(), which materializes the child's
// local deltas as a HogDB subtransaction and logs the boolean outcome as a
// nondeterministic bit.  In validation mode finish_frame() consumes that bit
// from NondeterministicProposalData and routes control flow on it (success vs.
// failure branch); it does NOT replay the child-boundary HogDB admission.  The
// only extra validation on a true bit is a narrow check that any RCQ
// observation-bearing key is still an RCQ/unmodified view (see
// invocation_frame.cc finish_frame, the ValidationModeData branch).
//
// Consequently a proposer can forge the bit for a child whose only
// proposal-mode failure was a storage-balance lock (no RCQ observation
// involved): a forged true bit makes validation take the child-success branch
// even though honest proposal recorded failure, and a forged false bit makes
// validation take the child-failure branch even though honest proposal recorded
// success.
//
// Why this is tolerated (working-as-intended).
//   - The TryInvoke bit is a nondeterministic value the proposer is *trusted*
//     to supply; validation accepts it as long as every invariant still holds
//     at the end of the transaction.  The per-child admission probe is an early
//     two-phase-commit optimization, not a security boundary: it only exists
//     because the frame carries a TryInvoke savepoint.  Without TryInvoke there
//     would be no per-child conflict check at all.
//   - SystemCallHandler::try_commit_transaction() submits ONE aggregate
//     subtransaction for the final accumulated root frame.  Child writes are
//     merged into the parent and later same-key writes/deletes/transfers
//     collapse into that single batch (commit_successful_child_frame +
//     invoke_tx.cc balance/storage netting).  HogDB re-applies the real
//     storage-balance admission to that aggregate.  So a forged bit can only
//     commit state that the genuine aggregate admission already accepts as
//     affordable — no free storage, no negative balance, no broken invariant.
//   - Forging the bit therefore only lets the proposer *choose a control-flow
//     branch* (which the proposer can already do by constructing the
//     transaction differently).  It cannot persist state that an honest
//     aggregate root commit would reject.
//
// This file pins the storage and branch-selection consequences of that gap.
// The amendments on the source issue describe the same root cause carrying
// other child payloads through commit_successful_child_frame() — events,
// deployment metadata, returned auth tokens, the child return buffer — and an
// overwrite-instead-of-delete variant of the netting below.  All share this
// tolerance rationale: the merged payload still rides the one aggregate root
// commit, which enforces the real invariants.
//
// See: https://github.com/groundhogchain/hogd/issues/2371

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "built_contracts/itc_erc20.h"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/precompiles/precompile_native_token.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
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

namespace hogd {

// NOLINTBEGIN(bugprone-unchecked-optional-access,bugprone-exception-escape)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class TryInvokeAdmissionTrustedTest : public ::testing::Test,
                                      public NativeContractTestFramework {
public:
  TryInvokeAdmissionTrustedTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

protected:
  // Parent (root invoked address), unfundable child, native token precompile.
  static constexpr int32_t kRootNativeId = 0;
  static constexpr int32_t kChildNativeId = 2;
  static constexpr int32_t kTokenNativeId = 3;
  static constexpr ContractAddress kAddrRoot = make_contract_address(0);
  static constexpr ContractAddress kAddrChild = make_contract_address(2);
  static constexpr ContractAddress kAddrToken = make_contract_address(3);

  static constexpr Runtime::MethodID kChildWrite = 0;
  static constexpr Runtime::MethodID kChildDelete = 1;

  static std::tuple<uint64_t, uint64_t> make_amount(uint128_t amount)
  {
    return std::make_tuple(static_cast<uint64_t>(amount >> 64),
                           static_cast<uint64_t>(amount &
                                                 0xffffffffffffffffULL));
  }

  // Build a NativeContract from a list of method lambdas (IDs 0, 1, ...).
  template<typename... Fns>
  static std::unique_ptr<NativeContract> make_contract(Fns &&...fns)
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> map;
    Runtime::MethodID id = 0;
    (map.emplace(id++, std::forward<Fns>(fns)), ...);
    return std::make_unique<NativeContract>(std::move(map));
  }

  void deploy_native(ContractAddress const &addr, int32_t native_id)
  {
    auto xdr = make_native_contract(native_id);
    write_contract(xdr);
    write_deployment(addr, xdr);
  }

  Transaction make_invoke_root_tx()
  {
    return Transaction{.invocation =
                           Invocation{.invoked_address = kAddrRoot,
                                      .entrypoint =
                                          entrypoint_from_method_id(0),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = {}},
                       .uniquifier = make_new_uniquifier()};
  }

  // try_invoke child method `method` from inside a parent contract lambda.
  static auto try_invoke_child(NativeSyscallContext &ctx,
                               Runtime::MethodID method,
                               CopyableInvokeOutputs &outputs)
  {
    CopyableInvokeInputs inputs{
        .entrypoint_id = method,
        .gas_limit = UINT64_MAX,
        .flags = std::to_underlying(InvokeFlag::TryInvoke),
    };
    return ctx.syscall<SyscallNo::kInvoke>(kAddrChild,
                                           std::span<const std::byte>{},
                                           std::span<uint32_t>{}, inputs,
                                           outputs);
  }

  // Plain (non-try) invoke of child method `method`.
  static auto invoke_child(NativeSyscallContext &ctx, Runtime::MethodID method)
  {
    CopyableInvokeInputs inputs{
        .entrypoint_id = method,
        .gas_limit = UINT64_MAX,
        .flags = std::to_underlying(InvokeFlag::None),
    };
    return ctx.syscall<SyscallNo::kInvoke>(kAddrChild,
                                           std::span<const std::byte>{},
                                           std::span<uint32_t>{}, inputs,
                                           CopyableInvokeOutputs{});
  }

  // Child method: write `payload` to the child's own "child-key". The child has
  // no storage-token balance, so this fails proposal-mode child admission.
  static NativeContract::function_t make_child_write(std::string_view payload)
  {
    return [p = std::string(payload)](
               NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
      auto key = make_storage_key("child-key");
      auto [ws, _wg] = syscall_BytestringSet_inner(
          ctx.handler(), key, std::as_bytes(std::span(std::string_view(p))),
          std::to_underlying(StorageDeltaFlags::None));
      if (!ws.has_value() ||
          ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
        return InvokeError{InvokeErrorCode::Deterministic};
      }
      return {};
    };
  }

  // Child method: delete the child's own "child-key". Deleting a key that does
  // not exist (the proposal-mode case, where the child write was discarded)
  // succeeds as a no-op.
  static NativeContract::function_t make_child_delete()
  {
    return [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
      auto key = make_storage_key("child-key");
      auto del = ctx.syscall<SyscallNo::kDeleteObject>(key, 0);
      if (!del.has_value() ||
          del.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
        return InvokeError{InvokeErrorCode::Deterministic};
      }
      return {};
    };
  }
};

// ---------------------------------------------------------------------------
// Anomaly A: forged true bit + later native-token transfer.
//
// The child writes a bytestring it cannot afford (false bit in honest
// proposal). The parent then transfers enough native token to the child to
// cover that storage cost. With the bit forged to true, validation takes the
// child-success branch, the child write is merged, and the one aggregate root
// commit nets transfer-in against storage-out — so the write is genuinely paid
// for and persists. No invariant is violated; this is exactly the affordable
// batch the real aggregate admission accepts.
// ---------------------------------------------------------------------------
TEST_F(TryInvokeAdmissionTrustedTest,
       ForgedTrueBitChildWritePaidByLaterTransfer)
{
  constexpr std::string_view kPayload = "unfunded-child-write";

  auto const *cost_model = get_default_cost_model();
  int128_t transfer_amount = cost_model->get_bytestring_cost(kPayload.size()) +
                             cost_model->get_balance_cost();
  ASSERT_GT(transfer_amount, 0);
  auto transfer_amount_xdr =
      make_amount(static_cast<uint128_t>(transfer_amount));

  auto child = make_contract(make_child_write(kPayload));
  auto parent = make_contract(
      [transfer_amount_xdr](
          NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        CopyableInvokeOutputs outputs{};
        auto child_res = try_invoke_child(ctx, kChildWrite, outputs);
        if (!child_res.has_value()) {
          return InvokeError{InvokeErrorCode::Unrecoverable};
        }

        auto token = ContractTraitClient<itc_erc20::ERC20>(kAddrToken);
        auto transfer_res = token.invoke_opt<u8"transfer">(ctx, kAddrChild,
                                                           transfer_amount_xdr);
        if (!transfer_res.has_value() || !transfer_res.value().has_value() ||
            !transfer_res.value().value().has_value() ||
            !transfer_res.value().value().value().is_ok()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(kRootNativeId, std::move(parent));
  contract_map.emplace(kChildNativeId, std::move(child));
  contract_map.emplace(kTokenNativeId, make_native_token_contract());
  auto ctx = std::make_unique<NativeContext>(std::move(contract_map));

  deploy_native(kAddrRoot, kRootNativeId);
  deploy_native(kAddrChild, kChildNativeId);
  deploy_native(kAddrToken, kTokenNativeId);
  // Parent funds the transfer + its own replay-cache cost. Child gets nothing,
  // so its child-boundary admission fails in proposal.
  setup_grant_storage_token(kAddrRoot, 1'000'000'000);

  auto tx = make_invoke_root_tx();

  // Honest proposal: child admission fails (false bit) because the child cannot
  // pay for the write at the child boundary, before the parent's transfer.
  auto proposal_handler = make_new_system_call_handler(tx, *ctx);
  ASSERT_TRUE(proposal_handler.root_invoke().is_root_success());
  auto proposal = proposal_handler.get_proposal_data();
  ASSERT_EQ(proposal_handler.try_commit_transaction(),
            ExecResult::ExecResultOK);
  ASSERT_TRUE(proposal_handler.emit_events(
      ExecResult::ExecResultOK,
      ProposedTransaction{.signed_tx = SignedTransaction{.tx = tx}})());

  ASSERT_EQ(proposal.nondeterministic_bit_count, 1U);
  ASSERT_EQ(proposal.observed_queue_entry_sequence.size(), 0U);
  ASSERT_EQ(proposal.nondeterministic_bits[0] & uint8_t{1}, 0U)
      << "honest proposal records child admission failure (false) before the "
         "later transfer";

  // Forge the bit to true.
  auto doctored = proposal;
  doctored.nondeterministic_bits[0] |= uint8_t{1};

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  auto handler =
      make_new_system_call_handler_validation(tx, *ctx, std::move(doctored));
  ASSERT_TRUE(handler.root_invoke().is_root_success())
      << "validation trusts the forged true bit; the per-child HogDB admission "
         "is not replayed";
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK)
      << "the aggregate root batch is affordable: the transfer covers the "
         "child storage cost";
  ASSERT_TRUE(handler.emit_events(
      ExecResult::ExecResultOK,
      ProposedTransaction{.signed_tx = SignedTransaction{.tx = tx}})());

  advance_block();

  auto item = lookup_storage_item_from_db(
      make_contract_data_addrkey(kAddrChild, "child-key"));
  ASSERT_TRUE(item.has_value())
      << "the child write rides the affordable aggregate commit and persists";
  ASSERT_EQ(item.value().bytestring(), make_bytestring(kPayload));
}

// ---------------------------------------------------------------------------
// Anomaly B: forged true bit + later delete (the write is never paid for).
//
// The child writes a bytestring it cannot afford (false bit in honest
// proposal). After the child returns, the parent invokes the child again to
// delete that key. With the bit forged to true, validation merges the child
// write; the subsequent delete collapses with the create in the single
// aggregate root commit, so the net storage cost for the child is zero and the
// batch is affordable even though the child never had a balance. Nothing
// unpaid persists — the key is absent after commit.
// ---------------------------------------------------------------------------
TEST_F(TryInvokeAdmissionTrustedTest, ForgedTrueBitChildWriteDeletedNeverPaid)
{
  constexpr std::string_view kPayload = "unfunded-child-write";

  auto child = make_contract(make_child_write(kPayload), make_child_delete());
  auto parent = make_contract(
      [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        CopyableInvokeOutputs outputs{};
        auto child_res = try_invoke_child(ctx, kChildWrite, outputs);
        if (!child_res.has_value()) {
          return InvokeError{InvokeErrorCode::Unrecoverable};
        }
        // Delete the (forged-merged) child write before root commit.
        auto del = invoke_child(ctx, kChildDelete);
        if (!del.has_value() ||
            del.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(kRootNativeId, std::move(parent));
  contract_map.emplace(kChildNativeId, std::move(child));
  auto ctx = std::make_unique<NativeContext>(std::move(contract_map));

  deploy_native(kAddrRoot, kRootNativeId);
  deploy_native(kAddrChild, kChildNativeId);
  setup_grant_storage_token(kAddrRoot, 1'000'000'000);

  auto tx = make_invoke_root_tx();

  auto proposal_handler = make_new_system_call_handler(tx, *ctx);
  ASSERT_TRUE(proposal_handler.root_invoke().is_root_success());
  auto proposal = proposal_handler.get_proposal_data();
  ASSERT_EQ(proposal_handler.try_commit_transaction(),
            ExecResult::ExecResultOK);
  ASSERT_TRUE(proposal_handler.emit_events(
      ExecResult::ExecResultOK,
      ProposedTransaction{.signed_tx = SignedTransaction{.tx = tx}})());

  ASSERT_EQ(proposal.nondeterministic_bit_count, 1U);
  ASSERT_EQ(proposal.nondeterministic_bits[0] & uint8_t{1}, 0U)
      << "honest proposal records child admission failure (false)";

  auto doctored = proposal;
  doctored.nondeterministic_bits[0] |= uint8_t{1};

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  auto handler =
      make_new_system_call_handler_validation(tx, *ctx, std::move(doctored));
  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK)
      << "create-then-delete nets to zero storage cost in the aggregate batch";
  ASSERT_TRUE(handler.emit_events(
      ExecResult::ExecResultOK,
      ProposedTransaction{.signed_tx = SignedTransaction{.tx = tx}})());

  advance_block();

  auto item = lookup_storage_item_from_db(
      make_contract_data_addrkey(kAddrChild, "child-key"));
  ASSERT_FALSE(item.has_value())
      << "the deleted child write never persists; the child paid nothing "
         "because the net aggregate delta is empty";
}

// ---------------------------------------------------------------------------
// Anomaly C: forged false bit as a branch oracle.
//
// The symmetric direction. The child performs an affordable write (true bit in
// honest proposal). With the bit forged to false, validation takes the
// child-failure branch: the child write is discarded and the parent commits its
// fallback marker instead. The committed state differs from honest proposal,
// but both branches are states the proposer could have produced directly, and
// the aggregate root commit still enforces every invariant. The proposer merely
// selects which control-flow branch the parent takes.
// ---------------------------------------------------------------------------
TEST_F(TryInvokeAdmissionTrustedTest, ForgedFalseBitForcesParentFallbackBranch)
{
  // Child method 0: affordable write to its own "child-key".
  auto child = make_contract(make_child_write("affordable"));
  auto parent = make_contract(
      [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        CopyableInvokeOutputs outputs{};
        auto child_res = try_invoke_child(ctx, kChildWrite, outputs);
        if (!child_res.has_value()) {
          return InvokeError{InvokeErrorCode::Unrecoverable};
        }
        // A child that ran but rolled back also returns
        // kSyscallErrorCodeNone, so a committed child is identified by
        // frame_status == Return.
        bool child_committed =
            child_res.value() == SyscallErrorCode::kSyscallErrorCodeNone &&
            outputs.frame_status == InvocationFrameStatusCode::Return;
        std::string_view marker = child_committed ? "success" : "fallback";

        auto key = make_storage_key("root-branch");
        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), key,
            std::as_bytes(std::span(std::string_view(marker))),
            std::to_underlying(StorageDeltaFlags::None));
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(kRootNativeId, std::move(parent));
  contract_map.emplace(kChildNativeId, std::move(child));
  auto ctx = std::make_unique<NativeContext>(std::move(contract_map));

  deploy_native(kAddrRoot, kRootNativeId);
  deploy_native(kAddrChild, kChildNativeId);
  // Both addresses funded: the child write is affordable (true bit), and the
  // parent can pay for its branch marker.
  setup_grant_storage_token(kAddrRoot, 1'000'000'000);
  setup_grant_storage_token(kAddrChild, 1'000'000'000);

  auto tx = make_invoke_root_tx();

  auto proposal_handler = make_new_system_call_handler(tx, *ctx);
  ASSERT_TRUE(proposal_handler.root_invoke().is_root_success());
  auto proposal = proposal_handler.get_proposal_data();
  ASSERT_EQ(proposal_handler.try_commit_transaction(),
            ExecResult::ExecResultOK);
  ASSERT_TRUE(proposal_handler.emit_events(
      ExecResult::ExecResultOK,
      ProposedTransaction{.signed_tx = SignedTransaction{.tx = tx}})());

  ASSERT_EQ(proposal.nondeterministic_bit_count, 1U);
  ASSERT_EQ(proposal.nondeterministic_bits[0] & uint8_t{1}, uint8_t{1})
      << "honest proposal records child admission success (true); parent "
         "commits the success branch";

  // Forge the bit to false.
  auto doctored = proposal;
  doctored.nondeterministic_bits[0] &= static_cast<uint8_t>(~uint8_t{1});

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  auto handler =
      make_new_system_call_handler_validation(tx, *ctx, std::move(doctored));
  ASSERT_TRUE(handler.root_invoke().is_root_success())
      << "validation trusts the forged false bit and takes the child-failure "
         "branch";
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events(
      ExecResult::ExecResultOK,
      ProposedTransaction{.signed_tx = SignedTransaction{.tx = tx}})());

  advance_block();

  // The parent committed its fallback branch, and the child's affordable write
  // was dropped.
  auto branch = lookup_storage_item_from_db(
      make_contract_data_addrkey(kAddrRoot, "root-branch"));
  ASSERT_TRUE(branch.has_value());
  ASSERT_EQ(branch.value().bytestring(), make_bytestring("fallback"))
      << "the forged false bit steered the parent to its fallback branch";

  auto child_item = lookup_storage_item_from_db(
      make_contract_data_addrkey(kAddrChild, "child-key"));
  ASSERT_FALSE(child_item.has_value())
      << "the discarded child frame's affordable write did not persist";
}

// NOLINTEND(bugprone-unchecked-optional-access,bugprone-exception-escape)

} // namespace hogd
