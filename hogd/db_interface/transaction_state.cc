#include "transaction_state.h"

#include <cassert>
#include <optional>
#include <unordered_map>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/storage_item/storage_lock_util.h"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

TransactionState::TransactionState()
{
  subtransaction_stack_.emplace_back();
}

void
TransactionState::push_subtransaction()
{
  subtransaction_stack_.emplace_back();
}

void
TransactionState::pop_subtransactions(uint32_t count)
{
  assert(subtransaction_stack_.size() > count);
  subtransaction_stack_.resize(subtransaction_stack_.size() - count);
  assert(subtransaction_stack_.size() > 0);
}

void
TransactionState::add_delta(AddressAndKey const &addrkey, StorageDelta &&delta,
                            MeteredStorageLock &&new_lock)
{
  assert(!finished_);
  subtransaction_stack_.back().deltas_[addrkey] = std::move(delta);
  held_locks_[addrkey] = std::move(new_lock);
}

std::unordered_map<AddressAndKey,
                   std::pair<std::optional<StorageDelta>, MeteredStorageLock>>
TransactionState::extract_results()
{
  assert(!finished_);
  finished_ = true;

  std::unordered_map<AddressAndKey,
                     std::pair<std::optional<StorageDelta>, MeteredStorageLock>>
      result;

  for (size_t i = subtransaction_stack_.size(); i > 0; --i) {
    auto &subtx = subtransaction_stack_[i - 1];
    for (auto &[addrkey, delta] : subtx.deltas_) {
      result.emplace(addrkey,
                     std::make_pair(std::move(delta), held_locks_.at(addrkey)));
    }
  }

  for (auto &[addrkey, lock] : held_locks_) {
    result.emplace(addrkey, std::make_pair(std::nullopt, std::move(lock)));
  }

  return result;
}

} // namespace hogd
