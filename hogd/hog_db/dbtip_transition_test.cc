// Preconditions on advancing the committed tip (#3873).
//
// [RPCContext::check_dbtip_transition] is the sole reason [update_dbtip]
// refuses a commit, and [HogDbSchedRpc::commit_block] runs it before it
// applies a block's state so that a refusal costs nothing.

#include <gtest/gtest.h>

#include "consensus.hh"
#include "rpc_types.hh"
#include "types.hh"

#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/status.h"

namespace hogd {

namespace {

constexpr TimePoint kTipTimepoint = 1'700'000'000;

DbTip
make_tip(BlockHeight height, TimePoint timepoint)
{
  DbTip tip{};
  tip.id = 7;
  tip.value.proposal_header.block_height = height;
  tip.value.proposal_header.block_timepoint = timepoint;
  tip.value.proposal_header.network_hash.fill(0xab);
  return tip;
}

ConsensusValue
next_value(const DbTip &tip, TimePoint timepoint)
{
  ConsensusValue value = tip.value;
  value.proposal_header.block_height =
      tip.value.proposal_header.block_height + 1;
  value.proposal_header.block_timepoint = timepoint;
  return value;
}

} // namespace

TEST(DbTipTransitionTest, AcceptsAdvancingTimepoint)
{
  const DbTip tip = make_tip(/*height=*/4, kTipTimepoint);
  EXPECT_TRUE(RPCContext::check_dbtip_transition(
                  /*bid=*/8, next_value(tip, kTipTimepoint + 1), tip)
                  .has_value());
}

// Two blocks sharing a nanosecond is implausible, not incoherent.
TEST(DbTipTransitionTest, AcceptsEqualTimepoint)
{
  const DbTip tip = make_tip(/*height=*/4, kTipTimepoint);
  EXPECT_TRUE(RPCContext::check_dbtip_transition(
                  /*bid=*/8, next_value(tip, kTipTimepoint), tip)
                  .has_value());
}

// The clock-skew case from #3873: a proposer whose clock ran backwards. This
// is refused, and [HogDbSchedRpc::commit_block] asks before it applies any
// state, so the DB is not left carrying a block the tip does not name.
TEST(DbTipTransitionTest, RejectsBackwardsTimepoint)
{
  const DbTip tip = make_tip(/*height=*/4, kTipTimepoint);
  const HogDbStatus<void> result = RPCContext::check_dbtip_transition(
      /*bid=*/8, next_value(tip, kTipTimepoint - 1), tip);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), HogDbCode::kInternal);
}

TEST(DbTipTransitionTest, RejectsMismatchedNetworkHash)
{
  const DbTip tip = make_tip(/*height=*/4, kTipTimepoint);
  ConsensusValue value = next_value(tip, kTipTimepoint + 1);
  value.proposal_header.network_hash.fill(0xcd);
  const HogDbStatus<void> result =
      RPCContext::check_dbtip_transition(/*bid=*/8, value, tip);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), HogDbCode::kInternal);
}

// The tip before genesis is at height -1 and carries neither a network hash
// nor a timepoint to compare against, so genesis installs both unchallenged.
TEST(DbTipTransitionTest, PreGenesisTipHasNothingToCompare)
{
  const DbTip tip = make_tip(/*height=*/-1, kTipTimepoint);
  ConsensusValue value = next_value(tip, kTipTimepoint - 1);
  value.proposal_header.network_hash.fill(0xcd);
  EXPECT_TRUE(
      RPCContext::check_dbtip_transition(/*bid=*/8, value, tip).has_value());
}

} // namespace hogd
