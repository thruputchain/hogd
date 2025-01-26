#include "hogd/hogdb/softstate/block_state.h"

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
  parent_state_ = nullptr;
}

bool
BlockState::try_reserve_storage_delta(TransactionID txid,
                                      AddressAndKey const &addrkey,
                                      StorageDelta const &delta,
                                      RPCFlagSet flags)
{
  auto lock = transaction_cache_.get_lock(txid, addrkey);

  auto res =
      storage_view_.try_reserve_storage_delta(addrkey, delta, lock, flags);

  if (res) {
    transaction_cache_.add_delta(txid, addrkey, delta, *res, flags);
  }
  return res.has_value();
}

void
BlockState::commit_transaction(TransactionID txid)
{
  auto deltas = transaction_cache_.extract_deltas(txid);
  for (auto const &[addr, entry] : deltas) {
    if (entry.valid_for_final_commit) {
      storage_view_.commit_storage_delta(addr, entry.delta, entry.lock);
    }
    else {
      storage_view_.revert_storage_lock(addr, entry.lock);
    }
  }
}

void
BlockState::abort_transaction(TransactionID txid)
{
  auto deltas = transaction_cache_.extract_deltas(txid);
  for (auto const &[addr, entry] : deltas) {
    storage_view_.revert_storage_lock(addr, entry.lock);
  }
}

std::optional<StorageItemWrapper>
BlockState::get_snapshot_storage_item(AddressAndKey const &addrkey)
{
  return storage_view_.get_snapshot_storage_item(addrkey);
}

void
BlockState::commit_contract_create(Contract const &contract)
{
  storage_view_.commit_contract_create(contract);
}

void
BlockState::commit_contract_deploy(ContractAddress const &addr,
                                   SHA256Hash const &contract_hash)
{
  storage_view_.commit_contract_deploy(addr, contract_hash);
}

std::optional<SHA256Hash>
BlockState::get_snapshot_deployment(ContractAddress const &addr)
{
  return storage_view_.get_snapshot_deployment(addr);
}

std::shared_ptr<const Contract>
BlockState::get_snapshot_contract(SHA256Hash const &contract_hash)
{
  return storage_view_.get_snapshot_contract(contract_hash);
}

} // namespace hogd
