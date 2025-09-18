#include "hogd/db_interface/block_state.h"

#include <cassert>
#include <optional>
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/mockdb/in_memory_database.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_delta_flags.hh"
#include "storage_item.hh"
#include "storage_lock.hh" // NOLINT(misc-include-cleaner)
#include "types.hh"

namespace hogd {

void
BlockState::unlink_parent(InMemoryDatabase *base_database)
{
  // parent_state_ owns the parent StorageView inside storage_view_.
  // But storage_view, whenever this parent is accessed for reading,
  // holds a shared_mutex in shared mode.
  //
  // unlink_parent() acquires that mutex in exclusive mode,
  // so by the time unlink_parent returns,
  // any pending accesses to the storage_view parent
  // have finished.
  storage_view_.unlink_parent(base_database);
}

void
BlockState::commit_to_database()
{
  assert(is_closed());
  assert(!is_committed_to_database_);
  is_committed_to_database_ = true;
  storage_view_.commit_to_database();
}

bool
BlockState::try_reserve_storage_delta(TransactionID txid,
                                      AddressAndKey const &addrkey,
                                      StorageDelta delta,
                                      StorageItemRPCFlagSet flags)
{
  assert(!is_closed());
  auto [tx, tx_lock] = tx_manager_.get(txid);

  int128_t marginal_cost = 0;
  {
    auto lock = tx.get_lock(addrkey);

    auto res =
        storage_view_.try_reserve_storage_delta(addrkey, delta, lock, flags);

    if (!res) {
      return false;
    }
    marginal_cost = res->locked_tokens_ - tx.get_cost(addrkey);
    tx.add_delta(addrkey, std::move(delta), std::move(*res));
  }

  if (addrkey.key.key_type() ==
      StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance) {
    // no storage cost paid for this key
    return true;
  }

  AddressAndKey cost_addrkey =
      AddressAndKey{.address = addrkey.address,
                    .key = StorageKey(
                        StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};

  auto prev_delta = tx.get_delta(cost_addrkey);
  auto storage_lock = tx.get_lock(cost_addrkey);
  StorageDelta new_storage_delta;

  auto get_add_value = [](auto &delta) -> auto & {
    if (delta.type() == StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd) {
      return delta.nnint().add_value;
    }
    assert(delta.type() == StorageDeltaType::kDeltaTypeNonnegativeIntAdd);
    return delta.nnint_add();
  };

  if (prev_delta) {
    assert(prev_delta->type() ==
               StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd ||
           prev_delta->type() == StorageDeltaType::kDeltaTypeNonnegativeIntAdd);
    assert(storage_lock.lock_type() != StorageLockType::kNone);
    new_storage_delta = *prev_delta;
    get_add_value(new_storage_delta) =
        to_xdr(from_xdr(get_add_value(new_storage_delta)) - marginal_cost);
  }
  else {
    assert(storage_lock.lock_type() == StorageLockType::kNone ||
           storage_lock.lock_type() == StorageLockType::kSetItemTypeClass);
    new_storage_delta =
        StorageDelta(StorageDeltaType::kDeltaTypeNonnegativeIntAdd);
    new_storage_delta.nnint_add() = to_xdr(-marginal_cost);
  }

  auto new_lock = storage_view_.try_reserve_storage_delta(
      cost_addrkey, new_storage_delta, storage_lock,
      std::to_underlying(StorageItemRPCFlags::kStorageNone));

  if (!new_lock) {
    // No need to manually revert the lock on addrkey acquired above;
    // if this fails then the entire subtransaction will be reverted.
    return false;
  }

  tx.add_delta(cost_addrkey, std::move(new_storage_delta),
               std::move(*new_lock));

  return true;
}

void
BlockState::commit_transaction(TransactionID txid)
{
  assert(!is_closed());
  auto [tx, tx_lock] = tx_manager_.get(txid);
  auto deltas = tx.extract_results();

  for (auto const &[addr, entry] : deltas) {
    auto const &[delta, lock] = entry;
    int128_t released_amount = 0;
    if (delta) {
      released_amount = storage_view_.commit_storage_delta(addr, *delta, lock);
    }
    else {
      storage_view_.revert_storage_lock(addr, lock.lock_);
      released_amount = lock.locked_tokens_;
    }
    if (addr.key.key_type() ==
        StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance) {
      // no storage cost paid for this key
      continue;
    }

    assert(released_amount >= 0);
    auto cost_addrkey = AddressAndKey{
        .address = addr.address,
        .key =
            StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};
    StorageDelta cost_release_delta(
        StorageDeltaType::kDeltaTypeNonnegativeIntAdd);
    cost_release_delta.nnint_add() = to_xdr(released_amount);

    auto r = storage_view_.try_reserve_storage_delta(
        cost_addrkey, cost_release_delta, StorageLock(StorageLockType::kNone),
        std::to_underlying(StorageItemRPCFlags::kStorageNone));
    assert(r.has_value());
    storage_view_.commit_storage_delta(cost_addrkey, cost_release_delta, *r);
  }
  tx_manager_.increment_closed();
}

void
BlockState::abort_transaction(TransactionID txid)
{
  assert(!is_closed());
  auto [tx, tx_lock] = tx_manager_.get(txid);
  auto deltas = tx.extract_results();
  for (auto const &[addr, entry] : deltas) {
    auto const &[_, lock] = entry;
    storage_view_.revert_storage_lock(addr, lock.lock_);
  }
  tx_manager_.increment_closed();
}

void
BlockState::commit_contract_create(Contract const &contract)
{
  assert(!is_closed());
  storage_view_.commit_contract_create(contract);
}

void
BlockState::push_subtransaction(TransactionID txid)
{
  auto [tx, tx_lock] = tx_manager_.get(txid);
  tx.push_subtransaction();
}

void
BlockState::pop_subtransactions(TransactionID txid, uint32_t count)
{
  auto [tx, tx_lock] = tx_manager_.get(txid);
  tx.pop_subtransactions(count);
}

Nondeterministic<std::optional<QueueEntry>>
BlockState::get_concurrent_retrocausal_queue_entry(AddressAndKey const &addrkey,
                                                   uint32_t index)
{
  return storage_view_.get_concurrent_retrocausal_queue_entry(addrkey, index);
}

void
BlockState::close_block()
{
  storage_view_.close_block();
  tx_manager_.assert_all_closed();
}

} // namespace hogd
