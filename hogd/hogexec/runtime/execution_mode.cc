#include "hogd/hogexec/runtime/execution_mode.h"

#include <format>

#include "x23/format.h"

namespace hogd {

namespace {
uint8_t
offset_bit(uint32_t idx)
{
  return 1 << (idx % 8);
}
} // namespace

void
ProposalModeData::log_nondeterministic_bit(bool bit)
{
  uint8_t byte_count = (proposal_data_.try_invoke_count / 8) + 1;
  if (byte_count != proposal_data_.try_invoke_results.size()) {
    assert(proposal_data_.try_invoke_results.size() + 1 == byte_count);
    proposal_data_.try_invoke_results.emplace_back(0);
  }

  assert(!proposal_data_.try_invoke_results.empty());

  if (bit) {
    uint8_t &last_byte = proposal_data_.try_invoke_results.back();
    last_byte |= offset_bit(proposal_data_.try_invoke_count);
  }

  proposal_data_.try_invoke_count++;
}

void
ProposalModeData::log_queue_entry_observation(AddressAndKey const &addrkey,
                                              uint64_t sequence_no,
                                              QueueEntry const &entry)
{
  auto &vec = queue_entry_observations_[addrkey];
  assert(vec.size() >= sequence_no);

  if (vec.size() == sequence_no) {
    vec.push_back(entry);
    proposal_data_.observed_queue_entry_sequence.push_back(entry);
  }
  else {
    assert(vec[sequence_no] == entry);
  }
}

std::optional<QueueEntry>
ValidationModeData::get_queue_entry_observation(AddressAndKey const &addrkey,
                                                uint64_t sequence_no)
{
  auto &vec = queue_entry_observations_[addrkey];
  assert(vec.size() >= sequence_no);

  if (sequence_no < vec.size()) {
    // re-observing a cached observation.  E.g. the case of consume() then
    // revert() then consume().
    return vec[sequence_no];
  }

  // otherwise, observing a new queue entry

  if (next_queue_entry_index_ >=
      proposal_data_.observed_queue_entry_sequence.size()) {
    return std::nullopt;
  }

  auto const &out =
      proposal_data_.observed_queue_entry_sequence[next_queue_entry_index_];
  next_queue_entry_index_++;
  queue_entry_observations_[addrkey].push_back(out);
  return out;
}

std::optional<bool>
ValidationModeData::consume_nondeterministic_bit()
{
  if (next_try_invoke_index_ >= proposal_data_.try_invoke_count) {
    return std::nullopt;
  }

  size_t byte_idx = next_try_invoke_index_ / 8;
  if (proposal_data_.try_invoke_results.size() <= byte_idx) {
    return std::nullopt;
  }

  bool out = (proposal_data_.try_invoke_results[byte_idx] &
              offset_bit(next_try_invoke_index_)) != 0;
  next_try_invoke_index_++;
  return out;
}

bool
ValidationModeData::check_all_consumed() const
{
  if (proposal_data_.try_invoke_count != next_try_invoke_index_)
    return false;
  if (proposal_data_.try_invoke_results.size() !=
      (proposal_data_.try_invoke_count / 8) +
          (proposal_data_.try_invoke_count % 8 == 0 ? 0 : 1))
    return false;
  if (next_queue_entry_index_ !=
      proposal_data_.observed_queue_entry_sequence.size())
    return false;
  return true;
}

} // namespace hogd
