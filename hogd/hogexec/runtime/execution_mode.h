#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

#include "transaction.hh"

namespace hogd {

enum class ExecutionMode : uint8_t { kProposing = 0, kValidating = 1 };

// Data structure for proposal mode execution that collects nondeterministic
// choices made during transaction execution
struct ProposalModeData {
  // Proposal data containing nondeterministic choices made during execution
  NondeterministicProposal proposal_data_{};

  // Records a nondeterministic boolean choice (e.g., does a try_invoke frame
  // succeed?) made during try_invoke
  void log_try_invoke_bit(bool bit);
};

// Data structure for validation mode execution that validates nondeterministic
// choices against a proposal
class ValidationModeData {
public:
  // Constructs validation data with proposal to validate against
  ValidationModeData(NondeterministicProposal proposal_data)
    : proposal_data_(std::move(proposal_data))
  {}

  // Consumes and returns the next try_invoke bit from the proposal
  // Returns nullopt if no more bits are available
  std::optional<bool> consume_try_invoke_bit();

  // Returns true if all proposal bits have been consumed
  bool check_all_consumed() const;

private:
  // Proposal data containing expected nondeterministic choices
  const NondeterministicProposal proposal_data_;

  // Index of next try_invoke bit to consume from proposal
  uint32_t next_try_invoke_index_ = 0;
};

// Variant type that holds either proposal or validation mode data
using ExecutionModeData = std::variant<ProposalModeData, ValidationModeData>;

} // namespace hogd
