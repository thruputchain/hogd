#include <gtest/gtest.h>

#include "hogd/hogexec/runtime/execution_mode.h"
#include "retrocausal_queue.hh"
#include "transaction.hh"
#include "types.hh"

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

TEST(ExecutionModeTest, QueueEntryObservations)
{
  ProposalModeData proposal;
  AddressAndKey addrkey_1{.address = ContractAddress{1}, .key = StorageKey{1}};
  AddressAndKey addrkey_2{.address = ContractAddress{2}, .key = StorageKey{2}};

  // make same observation several times
  proposal.log_queue_entry_observation(addrkey_1, 0, QueueEntry{1});
  proposal.log_queue_entry_observation(addrkey_1, 0, QueueEntry{1});
  proposal.log_queue_entry_observation(addrkey_1, 0, QueueEntry{1});
  proposal.log_queue_entry_observation(addrkey_1, 0, QueueEntry{1});

  proposal.log_queue_entry_observation(addrkey_1, 1, QueueEntry{2});
  proposal.log_queue_entry_observation(addrkey_1, 2, QueueEntry{3});

  // log for a different address
  proposal.log_queue_entry_observation(addrkey_2, 0, QueueEntry{4});

  // repeat observations in a weird order
  proposal.log_queue_entry_observation(addrkey_1, 1, QueueEntry{2});
  proposal.log_queue_entry_observation(addrkey_1, 1, QueueEntry{2});
  proposal.log_queue_entry_observation(addrkey_1, 0, QueueEntry{1});

  EXPECT_EQ(proposal.proposal_data().observed_queue_entry_sequence.size(), 4);

  // check ordering of observations
  EXPECT_EQ(proposal.proposal_data().observed_queue_entry_sequence[0],
            QueueEntry{1});
  EXPECT_EQ(proposal.proposal_data().observed_queue_entry_sequence[1],
            QueueEntry{2});
  EXPECT_EQ(proposal.proposal_data().observed_queue_entry_sequence[2],
            QueueEntry{3});
  EXPECT_EQ(proposal.proposal_data().observed_queue_entry_sequence[3],
            QueueEntry{4});

  // check validation observations
  ValidationModeData validation(proposal.proposal_data());
  // Test assumes that the observations are in the same order as the proposal

  auto check_consume = [&](AddressAndKey addrkey, uint32_t index,
                           const QueueEntry &expected) {
    auto res = validation.consume_queue_entry_observation(addrkey, index);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->get(), expected);
  };

  check_consume(addrkey_1, 0, QueueEntry{1});
  check_consume(addrkey_1, 0, QueueEntry{1});
  check_consume(addrkey_1, 0, QueueEntry{1});
  check_consume(addrkey_1, 0, QueueEntry{1});

  check_consume(addrkey_1, 1, QueueEntry{2});
  check_consume(addrkey_1, 2, QueueEntry{3});
  check_consume(addrkey_2, 0, QueueEntry{4});

  check_consume(addrkey_1, 1, QueueEntry{2});
  check_consume(addrkey_1, 1, QueueEntry{2});
  check_consume(addrkey_1, 0, QueueEntry{1});

  EXPECT_TRUE(validation.check_all_consumed());
}

} // namespace hogd
