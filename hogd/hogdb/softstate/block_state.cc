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
                                      StorageDelta const &delta)
{
  auto res = storage_view_.try_reserve_storage_delta(addrkey, delta);

  if (res) {
    transaction_cache_.add_delta(txid, addrkey, delta);
  }
  return res;
}

void
BlockState::commit_transaction(TransactionID txid)
{
  auto deltas = transaction_cache_.get_deltas(txid);
  for (auto const &[addr, delta] : deltas) {
    storage_view_.revert_storage_delta(addr, delta);
  }
}

void
BlockState::abort_transaction(TransactionID txid)
{
  auto deltas = transaction_cache_.get_deltas(txid);
  for (auto const &[addr, delta] : deltas) {
    storage_view_.commit_storage_delta(addr, delta);
  }
}

std::optional<StorageItem>
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
