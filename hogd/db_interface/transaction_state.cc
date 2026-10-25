#include "transaction_state.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/storage_item/storage_lock_util.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "types.hh"

namespace hogd {

void
TransactionState::push_subtransaction(uint32_t id)
{
  assert(!finished_);
  subtransaction_stack_.emplace_back().id_ = id;
}

RPCError
TransactionState::pop_subtransactions(uint32_t count)
{
  assert(!finished_);
  if (subtransaction_stack_.size() < count) [[unlikely]] {
    return RPCError::kRPCUnrecoverable;
  }
  subtransaction_stack_.resize(subtransaction_stack_.size() - count);
  return RPCError::kRPCNone;
}

RPCError
TransactionState::merge_subtransactions(std::span<const uint32_t> ids)
{
  assert(!finished_);
  std::vector<bool> merged(subtransaction_stack_.size());
  for (uint32_t id : ids) {
    auto it =
        std::ranges::find(subtransaction_stack_, id, &SubTransactionState::id_);
    if (it == subtransaction_stack_.end() ||
        it == subtransaction_stack_.begin()) [[unlikely]] {
      return RPCError::kRPCUnrecoverable;
    }
    auto pos = static_cast<size_t>(it - subtransaction_stack_.begin());
    if (merged[pos]) [[unlikely]] {
      return RPCError::kRPCUnrecoverable;
    }
    merged[pos] = true;
  }
  // Merge oldest to newest, so a later subtransaction's delta for a key
  // overwrites an earlier one's.
  size_t out = 0;
  for (size_t in = 0; in < subtransaction_stack_.size(); ++in) {
    if (merged[in]) {
      for (auto &[addrkey, delta] : subtransaction_stack_[in].deltas_) {
        subtransaction_stack_[out - 1].deltas_.insert_or_assign(addrkey,
                                                                std::move(
                                                                    delta));
      }
      continue;
    }
    if (out != in) {
      subtransaction_stack_[out] = std::move(subtransaction_stack_[in]);
    }
    ++out;
  }
  subtransaction_stack_.resize(out);
  return RPCError::kRPCNone;
}

void
TransactionState::add_delta(AddressAndKey const &addrkey, StorageDelta &&delta,
                            MeteredStorageLock &&new_lock)
{
  assert(!finished_);
  assert(!subtransaction_stack_.empty());
  if constexpr (is_debug_build()) {
    if (subtransaction_stack_.back().deltas_.contains(addrkey)) {
      assert(addrkey.key.key_type() == StorageKeyType::FreeNativeTokenBalance);
    }
  }
  subtransaction_stack_.back().deltas_[addrkey] =
      std::make_pair(std::move(delta), new_lock.locked_tokens_);
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
                     std::make_pair(std::move(delta.first),
                                    MeteredStorageLock{.lock_ = std::move(lock),
                                                       .locked_tokens_ =
                                                           cost}));
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
  {
    auto [state, ul] = tx_states_.find(txid);
    if (state) {
      return {*state, std::move(ul)};
    }
  }
  auto [ptr, lock] = tx_states_.insert(txid, TransactionState());
  new_transaction_count_.fetch_add(1, std::memory_order_release);
  return {*ptr, std::move(lock)};
}

} // namespace hogd
