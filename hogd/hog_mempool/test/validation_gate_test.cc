// Regression test for the validation-gate leak fixed in this change.
//
// [schedulercoord_validate_block()]'s [SchedulerValidationUnknownParentBlock]
// early return used to skip [decrement_num_validations_in_progress()],
// leaking the validation-gate increment. [close_validation_gate()] then waits
// forever for the count to reach zero, so every subsequent
// [schedulercoord_commit_block()] wedges. (The bench harness maps a timeout
// to success, so a wedged validator is silently masked in CI.)
//
// Without the fix this test HANGS at the commit below (caught by the CI test
// timeout); with the fix it completes in well under a second.

#include <gtest/gtest.h>

#include <utility>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_mempool/rpc_from_coord.h"
#include "hogd/hog_mempool/test/infra.h"
#include "hogd/hog_mempool/test/loadgen.h"
#include "hogd/hog_mempool/test/txs.h"
#include "proposal.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

namespace {

ConsensusValue
consensus_value(const DbTip &parent, const Proposal &proposal)
{
  return ConsensusValue{.protocol = 0,
                        .ext = {},
                        .prev_hash = sha256(parent.value),
                        .proposal_header = proposal.header,
                        .proposal_txs_hash = proposal_txs_compatibility_hash(
                            proposal.transactions),
                        .upgrades = {}};
}

} // namespace

TEST(ValidationGate, UnknownParentBlockReleasesValidationGate)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 32);

  const ValidatorID validator_id{};
  MempoolTestInfra server;
  server.mempool_coord_rpc().wait_for_auth_data(/*expected_height=*/0)();

  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(txs)())();
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  const DbTip db_tip = get_tip.db_tip();
  ProposalContext ctx{};
  ctx.protocol = 0;
  const SchedulerProposalResult res_a =
      server.mempool_coord_rpc().schedulercoord_propose_block(db_tip.id,
                                                              std::move(ctx))();
  ASSERT_EQ(res_a.outcome(), SchedulerProposalOutcome::SchedulerProposalOK);
  const Proposal proposal_a = res_a.proposed_block().proposal;

  // Validate against a parent id that is not the last committed block.
  const auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id + 1,
                                                               proposal_a)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationUnknownParentBlock);

  // The commit must not wedge on a leaked validation-gate increment.
  const SchedulerCommitOutcome outcome =
      server.mempool_coord_rpc().schedulercoord_commit_block(
          res_a.proposed_block().block_id,
          consensus_value(db_tip, proposal_a))();
  EXPECT_EQ(outcome, SchedulerCommitOutcome::SchedulerCommitOK);
}

} // namespace hogd
