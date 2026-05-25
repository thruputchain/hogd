#pragma once

#include <format>
#include <map>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "retrocausal_queue.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

enum class ExecutionMode : uint8_t {
  kProposing = 0,
  kValidating = 1,
  kViewing = 2
};

} // namespace hogd

template<> struct std::formatter<hogd::ExecutionMode> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const hogd::ExecutionMode &mode, auto &ctx)
  {
    switch (mode) {
    case hogd::ExecutionMode::kProposing:
      return format_to(ctx.out(), "ExecutionMode::kProposing");
    case hogd::ExecutionMode::kValidating:
      return format_to(ctx.out(), "ExecutionMode::kValidating");
    case hogd::ExecutionMode::kViewing:
      return format_to(ctx.out(), "ExecutionMode::kViewing");
    }
    return format_to(ctx.out(), "ExecutionMode::Unknown");
  }
};

namespace hogd {

using ProposalModeCtor = std::monostate;
using ValidationModeCtor = NondeterministicProposalData;

struct ViewingModeCtor {
  NondeterministicProposalData proposal_data_segment_;
};

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

  NondeterministicProposalData const &proposal_data() const
  {
    return proposal_data_;
  }

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
  ValidationModeData(ValidationModeData &&) noexcept = delete;
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
  // proposal.
  // Returns nullopt if no more queue entries are available (which is a
  // validation failure)
  std::optional<Nondeterministic<QueueEntry>>
  consume_queue_entry_observation(AddressAndKey const &addrkey, uint32_t index);

  x23::Vec<QueueEntry>
  get_observed_queue_entries(AddressAndKey const &addrkey, uint32_t count) const;

  // Returns true if entirety of the nondeterministic data has been consumed
  bool check_all_consumed() const;

  NondeterministicProposalData const &proposal_data() const
  {
    return proposal_data_;
  }

private:
  // Proposal data containing expected nondeterministic choices
  const NondeterministicProposalData proposal_data_;

  // Index of next nondeterministic bit to consume from proposal
  uint32_t next_nondeterministic_bit_index_ = 0;
  // Index of next queue entry to consume from proposal
  uint32_t next_queue_entry_index_ = 0;
  std::map<AddressAndKey, std::vector<QueueEntry>> queue_entry_observations_;
};

class ViewingModeData {
public:
  ViewingModeData(ViewingModeCtor &ctor)
    : partial_validation_data_(std::move(ctor.proposal_data_segment_))
  {}

  ViewingModeData(const ViewingModeData &) = delete;
  ViewingModeData &operator=(const ViewingModeData &) = delete;
  ViewingModeData(ViewingModeData &&) noexcept = delete;
  ViewingModeData &operator=(ViewingModeData &&) noexcept = delete;

  // WAI: view mode defaults missing TryInvoke bits to true (success).
  // Views are optimistic read-only snapshots; defaulting to the success branch
  // gives callers the most useful result without requiring nondeterministic
  // data that only proposal mode can produce. Callers who need to explore the
  // failure branch can supply an explicit false bit via the ndet parameter.
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  Nondeterministic<bool> view_try_invoke_bit()
  {
    auto bit = partial_validation_data_.consume_nondeterministic_bit();
    if (!bit.has_value()) {
      return true;
    }
    return bit.value();
  }

  Nondeterministic<bool> view_query_queue_observation_bit()
  {
    auto bit = partial_validation_data_.consume_nondeterministic_bit();
    if (!bit.has_value()) {
      // default to empty queue failing in view mode
      return false;
    }
    return bit.value();
  }

  // Returns nullopt if no more queue entries are available (which is a
  // validation failure)
  std::optional<Nondeterministic<QueueEntry>>
  view_queue_entry_observation(AddressAndKey const &addrkey, uint32_t index)
  {
    return partial_validation_data_.consume_queue_entry_observation(addrkey,
                                                                    index);
  }

  x23::Vec<QueueEntry>
  get_observed_queue_entries(AddressAndKey const &addrkey, uint32_t count) const
  {
    auto entries =
        partial_validation_data_.get_observed_queue_entries(addrkey, count);
    CHECK_EQ(entries.size(), count);
    return entries;
  }

private:
  ValidationModeData partial_validation_data_;
};

// Variant type that holds either proposal or validation mode data
using ExecutionModeData =
    std::variant<ProposalModeData, ValidationModeData, ViewingModeData>;

using ExecutionModeCtor =
    std::variant<ProposalModeCtor, ValidationModeCtor, ViewingModeCtor>;

} // namespace hogd
