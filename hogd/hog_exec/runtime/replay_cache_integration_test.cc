// Integration tests for replay-cache uniquifier consumption on the normal
// hog_exec transaction execution path
// (SystemCallHandler::try_commit_transaction).
//
// Every transaction included in a block consumes its Uniquifier into the
// replay cache, keyed by the root invoked address. This holds for the success,
// manual-revert, and deterministic-fault outcomes; only a replay-cache conflict
// (a duplicate or expired nonce) leaves the transaction not included.
//
// These run against HogDb; MockInMemory does not implement the replay cache.

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
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
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

// NOLINTBEGIN(bugprone-exception-escape,bugprone-unchecked-optional-access)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class ReplayCacheIntegrationTest : public ::testing::Test,
                                   public NativeContractTestFramework {
public:
  ReplayCacheIntegrationTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

  // method 0: success (no-op)
  // method 1: manual revert
  // method 2: deterministic fault
  // method 3: mark body execution, then success
  void SetUp() override
  {
    body_ran_ = false;

    auto contract_xdr = make_native_contract(0);
    write_contract(contract_xdr);
    write_deployment(kAddr, contract_xdr);
    setup_grant_storage_token(kAddr, 1'000'000'000);

    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(0,
                [](NativeSyscallContext & /*ctx*/)
                    -> x23::Async<InvokeStatus<void>> {
                  co_return InvokeStatus<void>{};
                });
    fns.emplace(
        1, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          ctx.handler().set_return_buffer(ReturnBuffer{},
                                          /*revert_frame=*/true);
          co_return InvokeStatus<void>{};
        });
    fns.emplace(2,
                [](NativeSyscallContext & /*ctx*/)
                    -> x23::Async<InvokeStatus<void>> {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                });
    fns.emplace(3,
                [this](NativeSyscallContext & /*ctx*/)
                    -> x23::Async<InvokeStatus<void>> {
                  body_ran_ = true;
                  co_return InvokeStatus<void>{};
                });

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
    cmap.emplace(0, std::make_unique<NativeContract>(std::move(fns)));
    ctx_ = std::make_unique<NativeContext>(std::move(cmap));
  }

  static constexpr ContractAddress kAddr = make_contract_address(7);
  static constexpr ContractAddress kControlAddr = make_contract_address(8);
  std::unique_ptr<NativeContext> ctx_;
  bool body_ran_ = false;

  static Transaction
  make_tx_for(ContractAddress const &addr, uint32_t method, uint64_t nonce,
              BlockHeight expiry = std::numeric_limits<BlockHeight>::max())
  {
    return Transaction{.invocation =
                           Invocation{.invoked_address = addr,
                                      .entrypoint =
                                          entrypoint_from_method_id(method),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = {}},
                       .uniquifier = Uniquifier{.block_expiry_height = expiry,
                                                .nonce = nonce}};
  }

  static Transaction
  make_tx(uint32_t method, uint64_t nonce,
          BlockHeight expiry = std::numeric_limits<BlockHeight>::max())
  {
    return make_tx_for(kAddr, method, nonce, expiry);
  }

  // Seed an address's persisted replay cache with `height_count` heights that
  // are all already expired at the current block (height < current height),
  // each holding a single nonce.
  void seed_expired_replay_cache(ContractAddress const &addr,
                                 std::int64_t height_count)
  {
    auto &dao = get_hog_db_dao();
    const auto current_height = static_cast<BlockHeight>(get_block_number());
    const BlockHeight first_height = current_height - height_count;

    ReplayCacheMetadata meta;
    meta.size = height_count;
    meta.heights.reserve(height_count);
    for (std::int64_t i = 0; i < height_count; ++i) {
      const BlockHeight h = first_height + static_cast<BlockHeight>(i);
      ASSERT_LT(h, current_height);
      meta.heights.push_back(
          ReplayCacheHeightCount{.height = h, .nonce_count = 1});

      ReplayCacheData data;
      data.nonces.push_back(static_cast<uint64_t>(i + 1));
      ASSERT_TRUE(dao.write_address_replay_cache_data(addr, h, data));
    }
    ASSERT_TRUE(dao.write_address_replay_cache_meta(addr, meta));
  }

  ExecResult run_and_commit(Transaction const &tx)
  {
    auto handler = make_new_system_call_handler(tx, *ctx_);
    handler.root_invoke()();
    return handler.try_commit_transaction()();
  }
};

TEST_F(ReplayCacheIntegrationTest, DistinctNoncesAccepted)
{
  EXPECT_EQ(run_and_commit(make_tx(0, 1)), ExecResult::ExecResultOK);
  EXPECT_EQ(run_and_commit(make_tx(0, 2)), ExecResult::ExecResultOK);
  EXPECT_EQ(run_and_commit(make_tx(0, 3)), ExecResult::ExecResultOK);
}

TEST_F(ReplayCacheIntegrationTest, DuplicateUniquifierRejected)
{
  EXPECT_EQ(run_and_commit(make_tx(0, 100)), ExecResult::ExecResultOK);
  // Same (nonce, expiry) again -> replay-cache conflict -> not included.
  EXPECT_EQ(run_and_commit(make_tx(0, 100)),
            ExecResult::ExecResultTxNonDeterministicError);
}

// A uniquifier whose block_expiry_height is in the past is rejected as an
// ExecResultTxValidationFailure -- uniformly in proposal and validation mode
// (perform_initial_validation produces it before the body runs).
TEST_F(ReplayCacheIntegrationTest, ExpiredHeightRejected)
{
  // Advance so block_expiry_height 0 is already in the past.
  advance_block();
  advance_block();
  EXPECT_EQ(run_and_commit(make_tx(0, /*nonce=*/9, /*expiry=*/0)),
            ExecResult::ExecResultTxValidationFailure);
}

// Future-proofing for #3188: the two uniquifiers that pack to a reserved
// ConcurrentSet sentinel -- all-zero {expiry=0, nonce=0} (kEmpty) and all-ones
// {expiry=-1, nonce=UINT64_MAX} (kTombstone) -- are wire-supplied inputs. At
// any user-reachable executing height they are caught by the expiry check
// (block_expiry_height < executing height) before the replay-cache reservation,
// so they are rejected end to end as ExecResultTxValidationFailure and never
// reach the sentinel path. This locks that rejection in even if the packing or
// expiry logic changes later; the reservation-side guard for the height-0
// boundary is covered by ReplayCacheLockTest.ReservedSentinelUniquifier*.
TEST_F(ReplayCacheIntegrationTest,
       ReservedSentinelUniquifiersRejectedOverTheWire)
{
  // Advance so any expiry <= 0 is strictly in the past regardless of the
  // initial executing height.
  advance_block();
  advance_block();

  // All-zero uniquifier packs to kEmpty.
  EXPECT_EQ(run_and_commit(make_tx(0, /*nonce=*/0, /*expiry=*/0)),
            ExecResult::ExecResultTxValidationFailure);

  // All-ones uniquifier packs to kTombstone.
  EXPECT_EQ(run_and_commit(make_tx(0, /*nonce=*/UINT64_MAX, /*expiry=*/-1)),
            ExecResult::ExecResultTxValidationFailure);
}

// An expired uniquifier is rejected during initial validation, before the root
// contract body can run (#2568). Previously the body executed and the tx was
// only rejected by the late replay-cache reservation. (Proposal mode.)
TEST_F(ReplayCacheIntegrationTest, ExpiredHeightRejectedBeforeBodyRuns)
{
  // Advance so expiry height 0 is already in the past.
  advance_block();
  advance_block();

  auto handler =
      make_new_system_call_handler(make_tx(3, /*nonce=*/10, /*expiry=*/0),
                                   *ctx_);
  auto root_res = handler.root_invoke()();
  EXPECT_FALSE(root_res.is_root_success());
  EXPECT_FALSE(body_ran_);
  EXPECT_EQ(handler.try_commit_transaction()(),
            ExecResult::ExecResultTxValidationFailure);
}

// An expired uniquifier combined with an invalid attached contract is still a
// ValidationFailure rejection, and the body never runs -- the expiry check
// returns before attached-contract verification could change the outcome to its
// Deterministic fault (#2568). (Proposal mode.)
TEST_F(ReplayCacheIntegrationTest,
       ExpiredHeightWithInvalidContractStillRejected)
{
  advance_block();
  advance_block();

  auto tx = make_tx(3, /*nonce=*/12, /*expiry=*/0);
  // Native contract id 99 is not registered in ctx_, so verify_contract() would
  // fault it as Deterministic if it ran after the expiry check.
  tx.contracts_to_deploy.push_back(*make_native_contract(99));

  auto handler = make_new_system_call_handler(tx, *ctx_);
  auto root_res = handler.root_invoke()();
  EXPECT_FALSE(root_res.is_root_success());
  EXPECT_FALSE(body_ran_);
  EXPECT_EQ(handler.try_commit_transaction()(),
            ExecResult::ExecResultTxValidationFailure);
}

// A non-expired uniquifier (default max expiry) still runs the body and
// commits, even after the block height has advanced.
TEST_F(ReplayCacheIntegrationTest, UnexpiredHeightRunsBodyAndCommits)
{
  advance_block();
  advance_block();

  auto handler = make_new_system_call_handler(make_tx(3, /*nonce=*/11), *ctx_);
  auto root_res = handler.root_invoke()();
  EXPECT_TRUE(root_res.is_root_success());
  EXPECT_TRUE(body_ran_);
  EXPECT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// The executor-side expiry comparison in replay_uniquifier_expired is strict
// (block_expiry_height < executing height): expiry exactly at the executing
// height runs the body and commits, one below is rejected. Pins the accept
// side against a < -> <= drift, which would reject transactions the db-side
// ReplayCacheLock (and other validators) accept.
TEST_F(ReplayCacheIntegrationTest, ExpiryAtExecutingHeightAcceptedBoundary)
{
  advance_block();
  advance_block();
  const auto executing_height = static_cast<BlockHeight>(get_block_number());

  EXPECT_EQ(run_and_commit(make_tx(0, /*nonce=*/21, executing_height - 1)),
            ExecResult::ExecResultTxValidationFailure);

  auto handler =
      make_new_system_call_handler(make_tx(3, /*nonce=*/22, executing_height),
                                   *ctx_);
  EXPECT_TRUE(handler.root_invoke()().is_root_success());
  EXPECT_TRUE(body_ran_);
  EXPECT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(ReplayCacheIntegrationTest, ManualRevertBurnsNonce)
{
  EXPECT_EQ(run_and_commit(make_tx(1, 200)),
            ExecResult::ExecResultTxRootFrameReverted);
  // The reverted transaction still burned its nonce: a replay is rejected.
  EXPECT_EQ(run_and_commit(make_tx(1, 200)),
            ExecResult::ExecResultTxNonDeterministicError);
}

TEST_F(ReplayCacheIntegrationTest, FaultBurnsNonce)
{
  EXPECT_EQ(run_and_commit(make_tx(2, 300)),
            ExecResult::ExecResultTxDeterministicError);
  // The faulting transaction still burned its nonce: a replay is rejected.
  EXPECT_EQ(run_and_commit(make_tx(2, 300)),
            ExecResult::ExecResultTxNonDeterministicError);
}

// A nonce burned by one outcome blocks replay regardless of the later attempt's
// outcome: consume via success, then a fault reusing the same uniquifier is
// rejected before it can execute.
TEST_F(ReplayCacheIntegrationTest, NonceSharedAcrossOutcomes)
{
  EXPECT_EQ(run_and_commit(make_tx(0, 400)), ExecResult::ExecResultOK);
  EXPECT_EQ(run_and_commit(make_tx(2, 400)),
            ExecResult::ExecResultTxNonDeterministicError);
}

// Expired replay-cache heights must not consume the active-height budget. An
// address sitting at exactly kReplayCacheHeightsMaxLen *expired* heights must
// still admit a fresh root uniquifier, and block close must prune the expired
// rows down to the single live height. Regression for #2581: get_base()
// previously counted expired snapshot heights against the cap, rejecting the
// fresh transaction that would have triggered close-time GC -- a circular
// lockout (no admission -> no GC -> no admission).
TEST_F(ReplayCacheIntegrationTest,
       ExpiredHeightCapStillAdmitsFreshRootUniquifierAndRunsGc)
{
  auto contract_xdr = make_native_contract(0);
  write_deployment(kControlAddr, contract_xdr);
  setup_grant_storage_token(kControlAddr, 1'000'000'000);

  auto &dao = get_hog_db_dao();

  // Control: one slot below the cap admits and prunes today and after the fix.
  seed_expired_replay_cache(kControlAddr, kReplayCacheHeightsMaxLen - 1);
  const BlockHeight control_expiry =
      static_cast<BlockHeight>(get_block_number()) + 100;
  EXPECT_EQ(run_and_commit(make_tx_for(kControlAddr, 0, 500, control_expiry)),
            ExecResult::ExecResultOK);
  advance_block();

  auto control_meta = dao.read_address_replay_cache_meta(kControlAddr);
  ASSERT_TRUE(control_meta);
  EXPECT_EQ(1, control_meta->size);
  ASSERT_EQ(1, control_meta->heights.size());
  EXPECT_EQ(control_expiry, control_meta->heights[0].height);
  EXPECT_EQ(1U, control_meta->heights[0].nonce_count);

  // Exactly at the cap with all-expired heights: the fresh uniquifier must
  // still be admitted because every snapshot height is expired.
  seed_expired_replay_cache(kAddr, kReplayCacheHeightsMaxLen);
  const BlockHeight live_expiry =
      static_cast<BlockHeight>(get_block_number()) + 100;
  EXPECT_EQ(run_and_commit(make_tx(0, 600, live_expiry)),
            ExecResult::ExecResultOK);

  advance_block();

  // Block close prunes the expired rows, leaving only the fresh live height.
  auto meta_after_close = dao.read_address_replay_cache_meta(kAddr);
  ASSERT_TRUE(meta_after_close);
  EXPECT_EQ(1, meta_after_close->size);
  ASSERT_EQ(1, meta_after_close->heights.size());
  EXPECT_EQ(live_expiry, meta_after_close->heights[0].height);
  EXPECT_EQ(1U, meta_after_close->heights[0].nonce_count);
}

// NOLINTEND(bugprone-exception-escape,bugprone-unchecked-optional-access)

} // namespace hogd
