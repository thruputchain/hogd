#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "storage_item.hh"
#include "transaction.hh"

namespace hogd {

enum class ExecutionMode : uint8_t { kProposing = 0, kValidating = 1 };

// Data structure for proposal mode execution that collects nondeterministic
// choices made during transaction execution
class ProposalModeData {
public:
  ProposalModeData() = default;
  ProposalModeData(const ProposalModeData &) = delete;
  ProposalModeData &operator=(const ProposalModeData &) = delete;
  ProposalModeData(ProposalModeData &&) = default;
  ProposalModeData &operator=(ProposalModeData &&) = delete;

  // Records a nondeterministic boolean choice (e.g., does a try_invoke frame
  // succeed?) made during try_invoke
  void log_nondeterministic_bit(bool bit);
  void log_queue_entry_observation(AddressAndKey const &addrkey,
                                   uint64_t sequence_no,
                                   QueueEntry const &entry);

  NondeterministicProposal proposal_data() const { return proposal_data_; }

private:
  // Proposal data containing nondeterministic choices made during execution
  NondeterministicProposal proposal_data_{};
  std::map<AddressAndKey, std::vector<QueueEntry>> queue_entry_observations_;
};

// Data structure for validation mode execution that validates nondeterministic
// choices against a proposal
class ValidationModeData {
public:
  ValidationModeData() = delete;
  ValidationModeData(const ValidationModeData &) = delete;
  ValidationModeData &operator=(const ValidationModeData &) = delete;
  ValidationModeData(ValidationModeData &&) = default;
  ValidationModeData &operator=(ValidationModeData &&) = delete;

  // Constructs validation data with proposal to validate against
  ValidationModeData(NondeterministicProposal proposal_data)
    : proposal_data_(std::move(proposal_data))
  {}

  // Consumes and returns the next nondeterministic bit from the proposal
  // Returns nullopt if no more bits are available
  std::optional<bool> consume_nondeterministic_bit();

  // Returns true if all proposal bits have been consumed
  bool check_all_consumed() const;

  std::optional<QueueEntry>
  get_queue_entry_observation(AddressAndKey const &addrkey,
                              uint64_t sequence_no);

private:
  // Proposal data containing expected nondeterministic choices
  const NondeterministicProposal proposal_data_;

  // Index of next try_invoke bit to consume from proposal
  uint32_t next_try_invoke_index_ = 0;
  uint32_t next_queue_entry_index_ = 0;

  std::map<AddressAndKey, std::vector<QueueEntry>> queue_entry_observations_;
};

// Variant type that holds either proposal or validation mode data
using ExecutionModeData = std::variant<ProposalModeData, ValidationModeData>;

} // namespace hogd
