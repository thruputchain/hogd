#pragma once

#include "hogd/protocol/block_compat.h"
#include "parallel.hh"
#include "sched_coord.hh"

namespace hogd {

struct Nomination {
  ValidatorPublicInfo proposer;
  BlockHeight block_height = 0;
  BlockHash block_hash{};
  BlockHash prev_block_hash{};
  ParallelSignedTransactionHashBatch transactions_hash;
  ParallelNondeterministicProposalDataBatch transactions_nondeterministic_data;
};

enum class SchedulerNominationOutcome {
  SchedulerNominationOK = 0,
  SchedulerNominationError = 1,
};

struct SchedulerNominatedBlock {
  BlockID block_id = 0;
  ParallelSignedTransactionBatch txs;
  Nomination nomination;
};

class SchedulerNominationResult {
public:
  SchedulerNominationResult()
    : outcome_(SchedulerNominationOutcome::SchedulerNominationOK)
  {}
  explicit SchedulerNominationResult(SchedulerNominationOutcome outcome)
    : outcome_(outcome)
  {}

  SchedulerNominationOutcome outcome() const { return outcome_; }
  SchedulerNominationResult &outcome(SchedulerNominationOutcome outcome)
  {
    outcome_ = outcome;
    return *this;
  }

  SchedulerNominatedBlock &nominated_block() { return nominated_block_; }
  const SchedulerNominatedBlock &nominated_block() const
  {
    return nominated_block_;
  }

private:
  SchedulerNominationOutcome outcome_;
  SchedulerNominatedBlock nominated_block_;
};

} // namespace hogd
