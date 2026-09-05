#include <utility>

#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/hog_exec/components/execution_mode.h"
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

namespace {
auto
make_storage_key(uint8_t value)
{
  StorageKey key(StorageKeyType::ContractData);
  key.contract_data_key() = StorageKeyContractData{value};
  return key;
}

// Distinct AddressAndKey per index, for filling the observation sequence past
// its uint8-limited make_storage_key range.
AddressAndKey
make_obs_key(uint32_t index)
{
  StorageKeyContractData data{};
  data[0] = static_cast<uint8_t>(index);
  data[1] = static_cast<uint8_t>(index >> 8U);
  data[2] = static_cast<uint8_t>(index >> 16U);
  data[3] = static_cast<uint8_t>(index >> 24U);
  StorageKey key(StorageKeyType::ContractData);
  key.contract_data_key() = data;
  return AddressAndKey{.address = ContractAddress{1}, .key = key};
}
} // namespace

TEST(ExecutionModeTest, QueueEntryObservations)
{
  ProposalModeData proposal;
  AddressAndKey addrkey_1{.address = ContractAddress{1},
                          .key = make_storage_key(1)};
  AddressAndKey addrkey_2{.address = ContractAddress{2},
                          .key = make_storage_key(2)};

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

  auto check_consume = [&](AddressAndKey const &addrkey, uint32_t index,
                           QueueEntry const &expected) {
    auto res = validation.consume_queue_entry_observation(addrkey, index);
    ASSERT_NE(res.get(), nullptr);
    EXPECT_EQ(*res.get(), expected);
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

TEST(ExecutionModeTest, ViewingModeDefaultsTryInvokeBitToTrue)
{
  ViewingModeCtor empty_ctor{.proposal_data_segment_ = {}};
  ViewingModeData viewing(empty_ctor);

  // WAI: missing bits default to true (success) — views are optimistic.
  auto bit = viewing.view_try_invoke_bit();
  EXPECT_TRUE(bit.get());
}

TEST(ExecutionModeTest, ViewingModeConsumesTryInvokeBitWhenSupplied)
{
  NondeterministicProposalData ndet{};
  ndet.nondeterministic_bits.push_back(0x02); // bits: false, true (LSB first)
  ndet.nondeterministic_bit_count = 2;

  ViewingModeCtor ctor{.proposal_data_segment_ = std::move(ndet)};
  ViewingModeData viewing(ctor);

  auto first = viewing.view_try_invoke_bit();
  EXPECT_FALSE(first.get());

  auto second = viewing.view_try_invoke_bit();
  EXPECT_TRUE(second.get());

  // Exhausted supplied bits; subsequent calls default to true.
  auto defaulted = viewing.view_try_invoke_bit();
  EXPECT_TRUE(defaulted.get());
}

// Guard against an overflow in bit count bug in an earlier version of hogd.
TEST(ExecutionModeTest, NondeterministicBitOverflowGuard)
{
  ProposalModeData proposal;
  for (size_t i = 0; i < (256 * 8); i++) {
    proposal.log_nondeterministic_bit(false);
  }

  ValidationModeData validation(proposal.proposal_data());
  ASSERT_TRUE(validation.validate());
}

// #2894: the (cap+1)th new concurrent observation flags the cap and records
// nothing, so the bounded observed_queue_entry_sequence never overflows.
TEST(ExecutionModeTest, ProposalObservationCapFlagsAndStopsRecording)
{
  ProposalModeData proposal;
  for (uint32_t i = 0; i < kMaxConcurrentRcqReadsPerTx; ++i) {
    proposal.log_queue_entry_observation(make_obs_key(i), 0, QueueEntry{1});
    ASSERT_FALSE(proposal.observation_cap_exceeded());
  }
  EXPECT_EQ(proposal.proposal_data().observed_queue_entry_sequence.size(),
            kMaxConcurrentRcqReadsPerTx);

  proposal.log_queue_entry_observation(make_obs_key(
                                           kMaxConcurrentRcqReadsPerTx),
                                       0, QueueEntry{1});
  EXPECT_TRUE(proposal.observation_cap_exceeded());
  EXPECT_EQ(proposal.proposal_data().observed_queue_entry_sequence.size(),
            kMaxConcurrentRcqReadsPerTx);
}

// #3043 follow-up: log_queue_entry_observation RETURNS a pointer to the
// recorded entry (nullptr at the cap), so the RCQ read path can only return an
// entry it actually recorded within the cap -- making a proposal-only
// cap-crossing consume structurally unrepresentable rather than merely guarded
// against.
TEST(ExecutionModeTest, ProposalObservationReturnsRecordedEntryThenNullAtCap)
{
  ProposalModeData proposal;

  // A fresh recording returns a pointer to the entry it recorded.
  for (uint32_t i = 0; i < kMaxConcurrentRcqReadsPerTx; ++i) {
    auto recorded =
        proposal.log_queue_entry_observation(make_obs_key(i), 0, QueueEntry{1});
    ASSERT_NE(recorded.get(), nullptr);
    EXPECT_EQ(*recorded.get(), QueueEntry{1});
  }

  // Re-observing an already-recorded (addrkey, index) returns the same entry
  // without advancing the sequence or tripping the cap.
  auto reobserved =
      proposal.log_queue_entry_observation(make_obs_key(0), 0, QueueEntry{1});
  ASSERT_NE(reobserved.get(), nullptr);
  EXPECT_EQ(*reobserved.get(), QueueEntry{1});
  EXPECT_FALSE(proposal.observation_cap_exceeded());

  // The cap-crossing observation records nothing and returns a wrapped nullptr,
  // so the caller has no entry to consume.
  auto capped = proposal.log_queue_entry_observation(
      make_obs_key(kMaxConcurrentRcqReadsPerTx), 0, QueueEntry{1});
  EXPECT_EQ(capped.get(), nullptr);
  EXPECT_TRUE(proposal.observation_cap_exceeded());
  EXPECT_EQ(proposal.proposal_data().observed_queue_entry_sequence.size(),
            kMaxConcurrentRcqReadsPerTx);
}

// #2894: validation replay must agree with proposal -- when the recorded
// sequence is exhausted at exactly the cap, a new observation flags the cap
// (which surfaces as InvalidSyscallInput), matching what proposal returned.
TEST(ExecutionModeTest, ValidationObservationCapMatchesProposal)
{
  ProposalModeData proposal;
  for (uint32_t i = 0; i < kMaxConcurrentRcqReadsPerTx; ++i) {
    proposal.log_queue_entry_observation(make_obs_key(i), 0, QueueEntry{1});
  }
  ValidationModeData validation(proposal.proposal_data());
  auto consume_ptr = [&](uint32_t key_index) {
    auto obs =
        validation.consume_queue_entry_observation(make_obs_key(key_index), 0);
    return obs.get();
  };
  for (uint32_t i = 0; i < kMaxConcurrentRcqReadsPerTx; ++i) {
    ASSERT_NE(consume_ptr(i), nullptr);
    ASSERT_FALSE(validation.observation_cap_exceeded());
  }
  EXPECT_EQ(consume_ptr(kMaxConcurrentRcqReadsPerTx), nullptr);
  EXPECT_TRUE(validation.observation_cap_exceeded());
}

// A recorded sequence shorter than the cap is a malformed proposal, not the
// cap case: exhausting it is a validation failure, and the cap flag stays off.
TEST(ExecutionModeTest, ValidationShortSequenceIsFailureNotCap)
{
  ProposalModeData proposal;
  proposal.log_queue_entry_observation(make_obs_key(0), 0, QueueEntry{1});
  proposal.log_queue_entry_observation(make_obs_key(1), 0, QueueEntry{2});
  ValidationModeData validation(proposal.proposal_data());
  auto consume_ptr = [&](uint32_t key_index) {
    auto obs =
        validation.consume_queue_entry_observation(make_obs_key(key_index), 0);
    return obs.get();
  };
  ASSERT_NE(consume_ptr(0), nullptr);
  ASSERT_NE(consume_ptr(1), nullptr);
  EXPECT_EQ(consume_ptr(2), nullptr);
  EXPECT_FALSE(validation.observation_cap_exceeded());
}

} // namespace hogd
