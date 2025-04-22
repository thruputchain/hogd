#include <gtest/gtest.h>

#include "hogd/hogexec/runtime/execution_mode.h"
#include "transaction.hh"

namespace hogd {

// check is simple but checks for an obvious mistake I made
// in an earlier version
TEST(ExecutionModeTest, ProposalBitStoredCorrectly)
{
  ProposalModeData proposal;

  // 10 bits, i.e. overflow 1 byte
  proposal.log_nondeterministic_bit(false);
  proposal.log_nondeterministic_bit(false);
  proposal.log_nondeterministic_bit(false);
  proposal.log_nondeterministic_bit(true);

  proposal.log_nondeterministic_bit(true);
  proposal.log_nondeterministic_bit(false);
  proposal.log_nondeterministic_bit(true);
  proposal.log_nondeterministic_bit(false);

  proposal.log_nondeterministic_bit(false);
  proposal.log_nondeterministic_bit(true);

  ValidationModeData validation(proposal.proposal_data());

  ASSERT_TRUE(validation.validate());

  auto check = [&](bool bit) {
    auto res = validation.consume_nondeterministic_bit();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->get(), bit);
  };

  check(false);
  check(false);
  check(false);
  check(true);

  check(true);
  check(false);
  check(true);
  check(false);

  check(false);
  check(true);

  ASSERT_FALSE(validation.consume_nondeterministic_bit());
  ASSERT_TRUE(validation.check_all_consumed());
}

TEST(ExecutionModeTest, ValidationModeEmptyCheck)
{
  NondeterministicProposalData proposal{};
  ValidationModeData validation(proposal);
  ASSERT_TRUE(validation.validate());
  ASSERT_TRUE(validation.check_all_consumed());
}

TEST(ExecutionModeTest, BadValidationCheck)
{
  NondeterministicProposalData bad_proposal{};
  bad_proposal.nondeterministic_bits.push_back(0xFF);
  bad_proposal.nondeterministic_bit_count = 2;
  ASSERT_FALSE(ValidationModeData(bad_proposal).validate());

  bad_proposal.nondeterministic_bit_count = 5;
  ASSERT_FALSE(ValidationModeData(bad_proposal).validate());

  bad_proposal.nondeterministic_bit_count = 0;
  ASSERT_FALSE(ValidationModeData(bad_proposal).validate());

  bad_proposal.nondeterministic_bit_count = 8;
  ASSERT_TRUE(ValidationModeData(bad_proposal).validate());

  bad_proposal.nondeterministic_bit_count = 9;
  ASSERT_FALSE(ValidationModeData(bad_proposal).validate());
}

} // namespace hogd
