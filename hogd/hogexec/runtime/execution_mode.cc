#include "hogd/hogexec/runtime/execution_mode.h"

#include <cassert>
#include <optional>
#include <utility>

#include "hogd/base/types.h"
#include "retrocausal_queue.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

namespace {
uint8_t
offset_bit(uint32_t idx)
{
  return 1 << (idx % 8);
}
} // namespace

void
ProposalModeData::log_nondeterministic_bit(Nondeterministic<bool> bit)
{
  uint8_t byte_count = (proposal_data_.nondeterministic_bit_count / 8) + 1;
  if (byte_count != proposal_data_.nondeterministic_bits.size()) {
    assert(proposal_data_.nondeterministic_bits.size() + 1 == byte_count);
    proposal_data_.nondeterministic_bits.emplace_back(0);
  }

  assert(!proposal_data_.nondeterministic_bits.empty());

  if (bit.get()) {
    uint8_t &last_byte = proposal_data_.nondeterministic_bits.back();
    last_byte |= offset_bit(proposal_data_.nondeterministic_bit_count);
  }

  proposal_data_.nondeterministic_bit_count++;
}

std::optional<Nondeterministic<bool>>
ValidationModeData::consume_nondeterministic_bit()
{
  if (next_nondeterministic_bit_index_ >=
      proposal_data_.nondeterministic_bit_count) {
    return std::nullopt;
  }

  size_t byte_idx = next_nondeterministic_bit_index_ / 8;
  if (proposal_data_.nondeterministic_bits.size() <= byte_idx) {
    return std::nullopt;
  }

  bool out = (proposal_data_.nondeterministic_bits[byte_idx] &
              offset_bit(next_nondeterministic_bit_index_)) != 0;
  next_nondeterministic_bit_index_++;
  return out;
}

void
ProposalModeData::log_queue_entry_observation(AddressAndKey addrkey,
                                              uint32_t index, QueueEntry entry)
{
  auto &entries = queue_entry_observations_[addrkey];
  if (entries.size() > index) {
    assert(entries[index] == entry);
    return;
  }
  // new observation: log to the entries list for this addrkey,
  // and push to the end of the overall nondeterministic sequence
  assert(entries.size() == index);
  entries.emplace_back(std::move(entry));
  proposal_data_.observed_queue_entry_sequence.emplace_back(entries.back());
}

std::optional<Nondeterministic<QueueEntry>>
ValidationModeData::consume_queue_entry_observation(AddressAndKey addrkey,
                                                    uint32_t index)
{
  auto &entries = queue_entry_observations_[addrkey];

  if (index < entries.size()) {
    // re-observing an entry
    return entries[index];
  }
  // observing a new entry
  if (next_queue_entry_index_ >=
      proposal_data_.observed_queue_entry_sequence.size()) {
    // validation failure -- no new entries to observe
    return std::nullopt;
  }
  auto out =
      proposal_data_.observed_queue_entry_sequence[next_queue_entry_index_];
  next_queue_entry_index_++;
  entries.push_back(out);
  return out;
}

bool
ValidationModeData::validate() const
{
  // assert the number of actual nondeterministic bits supplied
  // is at least the number that is claimed to be supplied
  if (proposal_data_.nondeterministic_bits.size() !=
      (proposal_data_.nondeterministic_bit_count / 8) +
          (proposal_data_.nondeterministic_bit_count % 8 == 0 ? 0 : 1))
    return false;

  // assert that any extra bits in proposal_data_.nondeterministic_bits are 0
  // (avoids one source of entropy grinding in proposer).
  if (!proposal_data_.nondeterministic_bits.empty()) {
    uint8_t last_byte = proposal_data_.nondeterministic_bits.back();
    auto trailing_zero_count = proposal_data_.nondeterministic_bit_count % 8;

    // if trailing zero count is 0, then any bit pattern on the last byte is
    // valid.
    if (trailing_zero_count != 0) {
      uint8_t last_byte_mask = 0xFF << trailing_zero_count;
      if ((last_byte & last_byte_mask) != 0)
        return false;
    }
  }

  if (next_queue_entry_index_ !=
      proposal_data_.observed_queue_entry_sequence.size()) {
    return false;
  }

  return true;
}

bool
ValidationModeData::check_all_consumed() const
{
  // have we consumed all available bits
  if (proposal_data_.nondeterministic_bit_count !=
      next_nondeterministic_bit_index_)
    return false;
  if (proposal_data_.nondeterministic_bits.size() !=
      (proposal_data_.nondeterministic_bit_count / 8) +
          (proposal_data_.nondeterministic_bit_count % 8 == 0 ? 0 : 1))
    return false;
  return true;
}

} // namespace hogd
