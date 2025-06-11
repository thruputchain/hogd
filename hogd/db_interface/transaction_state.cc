#include "transaction_state.h"

#include <atomic>
#include <cassert>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/storage_item/storage_lock_util.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "types.hh"

namespace hogd {

TransactionState::TransactionState()
{
  // TODO(geoff): updated interface doesn't need this
  subtransaction_stack_.emplace_back();
}

void
TransactionState::push_subtransaction()
{
  assert(!finished_);
  subtransaction_stack_.emplace_back();
}

void
TransactionState::pop_subtransactions(uint32_t count)
{
  assert(!finished_);
  assert(subtransaction_stack_.size() > count);
  subtransaction_stack_.resize(subtransaction_stack_.size() - count);
  assert(!subtransaction_stack_.empty());
}

void
TransactionState::add_delta(AddressAndKey const &addrkey, StorageDelta &&delta,
                            MeteredStorageLock &&new_lock)
{
  assert(!finished_);
  if constexpr (is_debug_build()) {
    if (subtransaction_stack_.back().deltas_.contains(addrkey)) {
      assert(addrkey.key.key_type() ==
             StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance);
    }
  }
  subtransaction_stack_.back().deltas_[addrkey] = std::make_pair(std::move(delta), new_lock.locked_tokens_);
  held_locks_[addrkey] = std::move(new_lock.lock_);
}

std::optional<StorageDelta>
TransactionState::get_delta(AddressAndKey const &addrkey) const
{
  assert(!finished_);
  for (size_t i = subtransaction_stack_.size(); i > 0; --i) {
    auto const &subtx = subtransaction_stack_[i - 1];
    auto it = subtx.deltas_.find(addrkey);
    if (it != subtx.deltas_.end()) {
      return it->second.first;
    }
  }
  return std::nullopt;
}

int128_t
TransactionState::get_cost(AddressAndKey const &addrkey) const
{
  assert(!finished_);
  for (size_t i = subtransaction_stack_.size(); i > 0; --i) {
    auto const &subtx = subtransaction_stack_[i - 1];
    auto it = subtx.deltas_.find(addrkey);
    if (it != subtx.deltas_.end()) {
      return it->second.second;
    }
  }
  return 0;
}

StorageLock
TransactionState::get_lock(AddressAndKey const &addrkey)
{
  assert(!finished_);
  auto it = held_locks_.find(addrkey);
  if (it == held_locks_.end()) {
    return StorageLock(StorageLockType::kNone);
  }
  return it->second;
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
      auto lock = held_locks_.at(addrkey);
      auto cost = delta.second;
      result.emplace(addrkey,
                     std::make_pair(std::move(delta.first), MeteredStorageLock{.lock_ = std::move(lock), .locked_tokens_ = cost}));
    }
  }

  for (auto &[addrkey, lock] : held_locks_) {
    result.emplace(addrkey, std::make_pair(std::nullopt, std::move(lock)));
  }

  return result;
}

std::pair<TransactionState &, std::unique_lock<std::mutex>>
TransactionStateManager::get(TransactionID txid)
{
  auto [state, ul] = tx_states_.find(txid);
  if (!state) {
    auto [ptr, lock] = tx_states_.insert(txid, TransactionState());
    new_transaction_count_.fetch_add(1, std::memory_order_release);
    return {*ptr, std::move(lock)};
  }
  return {*state, std::move(ul)};
}

} // namespace hogd
