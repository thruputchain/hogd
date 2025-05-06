#include "hogd/db_interface/block_state.h"

#include <cassert>
#include <optional>
#include <utility>

#include "contract.hh"
#include "hogd/mockdb/in_memory_database.h"
#include "rpc_types.hh"
#include "storage_item.hh"
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
  assert(is_closed());
  auto [tx, tx_lock] = tx_manager_.get(txid);

  auto lock = tx->get_lock(addrkey);

  auto res =
      storage_view_.try_reserve_storage_delta(addrkey, delta, lock, flags);

  if (res) {
    tx->add_delta(addrkey, std::move(delta), std::move(*res));
  }
  return res.has_value();
}

void
BlockState::commit_transaction(TransactionID txid)
{
  assert(!is_closed());
  auto [tx, tx_lock] = tx_manager_.get(txid);
  auto deltas = tx->extract_results();

  for (auto const &[addr, entry] : deltas) {
    auto const &[delta, lock] = entry;
    if (delta) {
      storage_view_.commit_storage_delta(addr, *delta, lock);
    }
    else {
      storage_view_.revert_storage_lock(addr, lock.lock_);
    }
  }
}

void
BlockState::abort_transaction(TransactionID txid)
{
  assert(!is_closed());
  auto [tx, tx_lock] = tx_manager_.get(txid);
  auto deltas = tx->extract_results();
  for (auto const &[addr, entry] : deltas) {
    auto const &[_, lock] = entry;
    storage_view_.revert_storage_lock(addr, lock.lock_);
  }
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

} // namespace hogd
