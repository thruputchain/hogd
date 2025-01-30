#include "hogd/hogexec/runtime/execution_mode.h"

namespace hogd {

namespace {
uint8_t
offset_bit(uint32_t idx)
{
  return 1 << (idx % 8);
}
} // namespace

void
ProposalModeData::log_try_invoke_bit(bool bit)
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

std::optional<bool>
ValidationModeData::consume_try_invoke_bit()
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
  return true;
}

} // namespace hogd
