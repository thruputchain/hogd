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

} // namespace hogd
