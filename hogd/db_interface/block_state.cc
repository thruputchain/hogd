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
#include "storage_item.hh"
#include "storage_lock.hh"
#include "types.hh"
#include "x23/format.h"

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
  if (prev_delta) {
    assert(prev_delta->type() ==
           StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd);
    assert(storage_lock.lock_type() != StorageLockType::kNone);
    new_storage_delta = *prev_delta;
    new_storage_delta.nnint().add_value =
        to_xdr(from_xdr(prev_delta->nnint().add_value) - marginal_cost);
  }
  else {
    assert(storage_lock.lock_type() == StorageLockType::kNone ||
           storage_lock.lock_type() == StorageLockType::kSetItemTypeClass);
    new_storage_delta =
        StorageDelta(StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd);
    new_storage_delta.nnint().add_value = to_xdr(-marginal_cost);

    auto const *snapshot =
        storage_view_.get_snapshot_storage_item(cost_addrkey);
    int128_t set_value = snapshot ? snapshot->nnint() : 0;
    new_storage_delta.nnint().set_value = to_xdr(set_value);
  }

  auto new_lock = storage_view_.try_reserve_storage_delta(
      cost_addrkey, new_storage_delta, storage_lock,
      std::to_underlying(StorageItemRPCFlags::kStorageNone));

  if (!new_lock) {
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
    if (delta) {
      storage_view_.commit_storage_delta(addr, *delta, lock);
    }
    else {
      storage_view_.revert_storage_lock(addr, lock.lock_);
    }
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
BlockState::commit_contract_deploy(ContractAddress const &addr,
                                   SHA256Hash const &contract_hash)
{
  assert(!is_closed());
  storage_view_.commit_contract_deploy(addr, contract_hash);
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
