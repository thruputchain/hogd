#include <cstdint>

#include <gtest/gtest.h>

#include "hogd/hogexec/runtime/execution_mode.h"

namespace hogd {

// check is simple but checks for an obvious mistake I made
// in an earlier version
TEST(ExecutionModeTest, ProposalBitStoredCorrectly)
{
  ProposalModeData proposal;

  // 10 bits, i.e. overflow 1 byte
  proposal.log_try_invoke_bit(false);
  proposal.log_try_invoke_bit(false);
  proposal.log_try_invoke_bit(false);
  proposal.log_try_invoke_bit(true);

  proposal.log_try_invoke_bit(true);
  proposal.log_try_invoke_bit(false);
  proposal.log_try_invoke_bit(true);
  proposal.log_try_invoke_bit(false);

  proposal.log_try_invoke_bit(false);
  proposal.log_try_invoke_bit(true);

  ValidationModeData validation(proposal.proposal_data_);

  auto check = [&](bool bit) {
    auto res = validation.consume_try_invoke_bit();
    ASSERT_TRUE(res);
    EXPECT_EQ(*res, bit);
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

  ASSERT_FALSE(validation.consume_try_invoke_bit());
  ASSERT_TRUE(validation.check_all_consumed());
}

} // namespace hogd
