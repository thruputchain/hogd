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

  // Records a concurrent queue-entry observation and RETURNS a
  // (nondeterministic) pointer to the recorded entry, so a caller can only
  // obtain an entry-to-consume as the output of recording it -- an unrecorded
  // entry is structurally unreturnable. Wraps nullptr when the observation
  // would exceed kMaxConcurrentRcqReadsPerTx: the entry is NOT recorded (the
  // cap flag is set instead), so the caller applies no consume and
  // proposal/validation stay in lockstep (#3043, #2894). The pointer aliases
  // into this ProposalModeData and is valid until the next observation is
  // recorded; callers use it before returning to the contract.
  Nondeterministic<const QueueEntry *>
  log_queue_entry_observation(AddressAndKey const &addrkey, uint32_t index,
                              QueueEntry entry);

  NondeterministicProposalData const &proposal_data() const
  {
    return proposal_data_;
  }

  // In proposal mode, observations don't need validation — the proposer
  // built the block from genuine concurrent data. Return empty so
  // try_acquire_frame_locks() emits no observation deltas.
  static std::map<AddressAndKey, std::vector<QueueEntry>> const &
  observations_to_validate()
  {
    static const std::map<AddressAndKey, std::vector<QueueEntry>> empty;
    return empty;
  }

  // Set once a concurrent observation would exceed kMaxConcurrentRcqReadsPerTx.
  // The RCQ read syscall then returns InvalidSyscallInput instead of
  // overflowing the bounded observed_queue_entry_sequence (#2894).
  bool observation_cap_exceeded() const { return observation_cap_exceeded_; }

  // Records a past-block RCQ read against the per-tx cap. A past-block
  // peek/consume pins a full decoded block (up to ~64 KiB) in the shared,
  // block-lifetime snapshot cache while charging zero direct host gas, and the
  // past-block read path bypasses the concurrent-observation cap; bounding the
  // count keeps a transaction from turning many cheap distinct-key reads into
  // unbounded host memory (#3531). Past-block reads are deterministic
  // committed-state reads, so the count matches in proposal and validation
  // without any observation logging -- this reuses kMaxConcurrentRcqReadsPerTx
  // and the existing RetrocausalQueueReadLimitReached resource-limit error.
  // Returns true (and counts the read) while under the cap; once the cap is
  // reached it sets the exceeded flag and returns false so the caller applies
  // no consume, keeping proposal and validation in lockstep.
  bool record_past_block_rcq_read()
  {
    if (past_block_rcq_read_count_ >= kMaxConcurrentRcqReadsPerTx) {
      past_block_rcq_read_cap_exceeded_ = true;
      return false;
    }
    ++past_block_rcq_read_count_;
    return true;
  }
  bool past_block_rcq_read_cap_exceeded() const
  {
    return past_block_rcq_read_cap_exceeded_;
  }

private:
  // Proposal data containing nondeterministic choices made during execution
  NondeterministicProposalData proposal_data_{};
  std::map<AddressAndKey, std::vector<QueueEntry>> queue_entry_observations_;
  bool observation_cap_exceeded_ = false;
  uint32_t past_block_rcq_read_count_ = 0;
  bool past_block_rcq_read_cap_exceeded_ = false;
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

  // Consumes the next nondeterministic queue entry from the proposal and
  // returns a (nondeterministic) pointer to it, or a wrapped nullptr if no more
  // queue entries are available (which is a validation failure). The pointer
  // aliases into this ValidationModeData and is valid until the next
  // observation is consumed; callers use it before returning to the contract.
  Nondeterministic<const QueueEntry *>
  consume_queue_entry_observation(AddressAndKey const &addrkey, uint32_t index);

  x23::Vec<QueueEntry> get_observed_queue_entries(AddressAndKey const &addrkey,
                                                  uint32_t count) const;

  // Returns true if entirety of the nondeterministic data has been consumed
  bool check_all_consumed() const;

  NondeterministicProposalData const &proposal_data() const
  {
    return proposal_data_;
  }

  std::map<AddressAndKey, std::vector<QueueEntry>> const &
  observations_to_validate() const
  {
    return queue_entry_observations_;
  }

  // Set when replay reaches a new observation but the recorded sequence is
  // exhausted at exactly kMaxConcurrentRcqReadsPerTx -- the proposer hit the
  // observation cap and surfaced InvalidSyscallInput, so replay must agree
  // (#2894). Distinguished from a malformed short sequence, which stays a
  // validation failure.
  bool observation_cap_exceeded() const { return observation_cap_exceeded_; }

  // See ProposalModeData::record_past_block_rcq_read. Past-block reads are
  // deterministic, so validation replays the same count and hits the cap at the
  // same read as the proposer did (#3531).
  bool record_past_block_rcq_read()
  {
    if (past_block_rcq_read_count_ >= kMaxConcurrentRcqReadsPerTx) {
      past_block_rcq_read_cap_exceeded_ = true;
      return false;
    }
    ++past_block_rcq_read_count_;
    return true;
  }
  bool past_block_rcq_read_cap_exceeded() const
  {
    return past_block_rcq_read_cap_exceeded_;
  }

private:
  // Proposal data containing expected nondeterministic choices
  const NondeterministicProposalData proposal_data_;

  // Index of next nondeterministic bit to consume from proposal
  uint32_t next_nondeterministic_bit_index_ = 0;
  // Index of next queue entry to consume from proposal
  uint32_t next_queue_entry_index_ = 0;
  std::map<AddressAndKey, std::vector<QueueEntry>> queue_entry_observations_;
  bool observation_cap_exceeded_ = false;
  uint32_t past_block_rcq_read_count_ = 0;
  bool past_block_rcq_read_cap_exceeded_ = false;
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
    return *bit;
  }

  Nondeterministic<bool> view_query_queue_observation_bit()
  {
    auto bit = partial_validation_data_.consume_nondeterministic_bit();
    if (!bit.has_value()) {
      // default to empty queue failing in view mode
      return false;
    }
    return *bit;
  }

  // Returns a (nondeterministic) pointer to the next observed queue entry, or a
  // wrapped nullptr if no more are available (a validation failure). Same
  // aliasing/lifetime as ValidationModeData::consume_queue_entry_observation.
  Nondeterministic<const QueueEntry *>
  view_queue_entry_observation(AddressAndKey const &addrkey, uint32_t index)
  {
    return partial_validation_data_.consume_queue_entry_observation(addrkey,
                                                                    index);
  }

  x23::Vec<QueueEntry> get_observed_queue_entries(AddressAndKey const &addrkey,
                                                  uint32_t count) const
  {
    auto entries =
        partial_validation_data_.get_observed_queue_entries(addrkey, count);
    CHECK_EQ(entries.size(), count);
    return entries;
  }

  std::map<AddressAndKey, std::vector<QueueEntry>> const &
  observations_to_validate() const
  {
    return partial_validation_data_.observations_to_validate();
  }

  bool observation_cap_exceeded() const
  {
    return partial_validation_data_.observation_cap_exceeded();
  }

  bool record_past_block_rcq_read()
  {
    return partial_validation_data_.record_past_block_rcq_read();
  }
  bool past_block_rcq_read_cap_exceeded() const
  {
    return partial_validation_data_.past_block_rcq_read_cap_exceeded();
  }

private:
  ValidationModeData partial_validation_data_;
};

// Variant type that holds either proposal or validation mode data
using ExecutionModeData =
    std::variant<ProposalModeData, ValidationModeData, ViewingModeData>;

// True once a concurrent RCQ observation would exceed
// kMaxConcurrentRcqReadsPerTx in the current mode (#2894).
inline bool
observation_cap_exceeded(ExecutionModeData const &mode)
{
  return std::visit(
      [](auto const &data) { return data.observation_cap_exceeded(); }, mode);
}

// Records a past-block RCQ read against the current mode's per-tx cap; returns
// false once the cap is reached (see
// ProposalModeData::record_past_block_rcq_read). (#3531)
inline bool
record_past_block_rcq_read(ExecutionModeData &mode)
{
  return std::visit(
      [](auto &data) { return data.record_past_block_rcq_read(); }, mode);
}

// True once a past-block RCQ read has been rejected for exceeding
// kMaxConcurrentRcqReadsPerTx in the current mode (#3531).
inline bool
past_block_rcq_read_cap_exceeded(ExecutionModeData const &mode)
{
  return std::visit(
      [](auto const &data) { return data.past_block_rcq_read_cap_exceeded(); },
      mode);
}

using ExecutionModeCtor =
    std::variant<ProposalModeCtor, ValidationModeCtor, ViewingModeCtor>;

} // namespace hogd
