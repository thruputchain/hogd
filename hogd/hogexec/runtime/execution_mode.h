#pragma once

#include <map>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "hogd/base/types.h"
#include "retrocausal_queue.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

enum class ExecutionMode : uint8_t { kProposing = 0, kValidating = 1 };

// Data structure for proposal mode execution that collects nondeterministic
// choices made during transaction execution
class ProposalModeData {
public:
  ProposalModeData() = default;

  // delete move and copy
  ProposalModeData(const ProposalModeData &) = delete;
  ProposalModeData &operator=(const ProposalModeData &) = delete;
  ProposalModeData(ProposalModeData &&) = default;
  ProposalModeData &operator=(ProposalModeData &&) = delete;

  // Records a nondeterministic boolean choice (e.g., does a try_invoke frame
  // succeed?) made during try_invoke
  void log_nondeterministic_bit(Nondeterministic<bool> bit);

  void log_queue_entry_observation(AddressAndKey const &addrkey, uint32_t index,
                                   QueueEntry entry);

  NondeterministicProposalData proposal_data() const { return proposal_data_; }

private:
  // Proposal data containing nondeterministic choices made during execution
  NondeterministicProposalData proposal_data_{};
  std::map<AddressAndKey, std::vector<QueueEntry>> queue_entry_observations_;
};

// Data structure for validation mode execution that validates nondeterministic
// choices against a proposal
class ValidationModeData {
public:
  ValidationModeData(const ValidationModeData &) = delete;
  ValidationModeData &operator=(const ValidationModeData &) = delete;
  ValidationModeData(ValidationModeData &&) noexcept = default;
  ValidationModeData &operator=(ValidationModeData &&) noexcept = delete;
  // Constructs validation data with proposal to validate against
  ValidationModeData(NondeterministicProposalData proposal_data)
    : proposal_data_(std::move(proposal_data))
  {}

  // Check whether the input NondeterministicProposalData is well-formed.
  // E.g. checks that the number of bits supplied in
  // nondeterministic_bits matches the expected number in
  // nondeterministic_bit_count.
  bool validate() const;

  // Consumes and returns the next nondeterministic bit from the proposal
  // Returns nullopt if no more bits are available (which is a validation
  // failure)
  std::optional<Nondeterministic<bool>> consume_nondeterministic_bit();

  // Consumes and returns the next nondeterministic queue entry from the
  // proposal Returns nullopt if no more queue entries are available (which is a
  // validation failure)
  std::optional<Nondeterministic<QueueEntry>>
  consume_queue_entry_observation(AddressAndKey const &addrkey, uint32_t index);

  // Returns true if entirety of the nondeterministic data has been consumed
  bool check_all_consumed() const;

private:
  // Proposal data containing expected nondeterministic choices
  NondeterministicProposalData proposal_data_;

  // Index of next nondeterministic bit to consume from proposal
  uint32_t next_nondeterministic_bit_index_ = 0;
  // Index of next queue entry to consume from proposal
  uint32_t next_queue_entry_index_ = 0;
  std::map<AddressAndKey, std::vector<QueueEntry>> queue_entry_observations_;
};

// Variant type that holds either proposal or validation mode data
using ExecutionModeData = std::variant<ProposalModeData, ValidationModeData>;

} // namespace hogd
