// Integration tests for replay-cache uniquifier consumption on the normal
// hogexec transaction execution path
// (SystemCallHandler::try_commit_transaction).
//
// Every transaction included in a block consumes its Uniquifier into the
// replay cache, keyed by the root invoked address. This holds for the success,
// manual-revert, and deterministic-fault outcomes; only a replay-cache conflict
// (a duplicate or expired nonce) leaves the transaction not included.
//
// These run against HogDB; MockInMemory does not implement the replay cache.

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
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
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// NOLINTBEGIN(bugprone-exception-escape,bugprone-unchecked-optional-access)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class ReplayCacheIntegrationTest : public ::testing::Test,
                                   public NativeContractTestFramework {
public:
  ReplayCacheIntegrationTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
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
                [](NativeSyscallContext & /*ctx*/) noexcept
                    -> InvokeStatus<void> { return {}; });
    fns.emplace(1,
                [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  ctx.handler().set_return_buffer(ReturnBuffer{},
                                                  /*revert_frame=*/true);
                  return {};
                });
    fns.emplace(
        2, [](NativeSyscallContext & /*ctx*/) noexcept -> InvokeStatus<void> {
          return InvokeError{InvokeErrorCode::Deterministic};
        });
    fns.emplace(3,
                [this](NativeSyscallContext & /*ctx*/) noexcept
                    -> InvokeStatus<void> {
                  body_ran_ = true;
                  return {};
                });

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
    cmap.emplace(0, std::make_unique<NativeContract>(std::move(fns)));
    ctx_ = std::make_unique<NativeContext>(std::move(cmap));
  }

  static constexpr ContractAddress kAddr = make_contract_address(7);
  std::unique_ptr<NativeContext> ctx_;
  bool body_ran_ = false;

  static Transaction
  make_tx(uint32_t method, uint64_t nonce,
          BlockHeight expiry = std::numeric_limits<BlockHeight>::max())
  {
    return Transaction{.invocation =
                           Invocation{.invoked_address = kAddr,
                                      .entrypoint =
                                          entrypoint_from_method_id(method),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = {}},
                       .uniquifier = Uniquifier{.block_expiry_height = expiry,
                                                .nonce = nonce}};
  }

  ExecResult run_and_commit(Transaction const &tx)
  {
    auto handler = make_new_system_call_handler(tx, *ctx_);
    handler.root_invoke();
    return handler.try_commit_transaction();
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
  auto root_res = handler.root_invoke();
  EXPECT_FALSE(root_res.is_root_success());
  EXPECT_FALSE(body_ran_);
  EXPECT_EQ(handler.try_commit_transaction(),
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
  auto root_res = handler.root_invoke();
  EXPECT_FALSE(root_res.is_root_success());
  EXPECT_FALSE(body_ran_);
  EXPECT_EQ(handler.try_commit_transaction(),
            ExecResult::ExecResultTxValidationFailure);
}

// A non-expired uniquifier (default max expiry) still runs the body and
// commits, even after the block height has advanced.
TEST_F(ReplayCacheIntegrationTest, UnexpiredHeightRunsBodyAndCommits)
{
  advance_block();
  advance_block();

  auto handler = make_new_system_call_handler(make_tx(3, /*nonce=*/11), *ctx_);
  auto root_res = handler.root_invoke();
  EXPECT_TRUE(root_res.is_root_success());
  EXPECT_TRUE(body_ran_);
  EXPECT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
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

// NOLINTEND(bugprone-exception-escape,bugprone-unchecked-optional-access)

} // namespace hogd
