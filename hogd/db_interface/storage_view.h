#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <variant>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/storage_item/invalid_state_flag.h"
#include "hogd/storage_item/storage_item_delta_manager.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "hogd/storage_item/storage_lock_util.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "types.hh"
#include "x23/mvar.h"

namespace hogd {

class InMemoryDatabase;

// Maintains one consistent view of the database state for a single block.
// Manages concurrent modifications (via try_reserve deltas) of transactions
// on the view.  When the block closes (i.e. call close_block()),
// then methods that view the state of the database _after_ the block
// (i.e. for a next block) can be called.
//
// The methods that change/add storage deltas cannot be called after
// close_block, and get_post_block_storage_item cannot be called
// before close_block.
//
// The view cannot have a child view opened until close_block is called.
//
// StorageView stores a database snapshot that has not yet been closed,
// and therefore makes public all methods necessary for modifying storage.
//
// A view contains exactly the information required, once it is closed,
// to produce a database transaction for one block.
// TODO(geoff): implementation of conversion of a view into a db transaction.
//
// Closing this block enables calling as_parent(), which produces a
// ParentBlockView that does not provide methods to modify storage,
// but does provide methods to read the state of storage _after_ that block.
class ParentBlockView {
public:
  // Disable move and copy
  ParentBlockView(ParentBlockView const &) = delete;
  ParentBlockView &operator=(ParentBlockView const &) = delete;
  ParentBlockView(ParentBlockView &&) = delete;
  ParentBlockView &operator=(ParentBlockView &&) = delete;

  // The value of the item, after all modifications of the current block are
  // applied. Cannot be called before the block is closed.
  std::shared_ptr<const StorageItemWrapper>
  get_post_block_storage_item(AddressAndKey const &addrkey);

  // Read from post block snapshot data, after all operations in the block
  // are applied.
  // Cannot be called before the block is closed.
  std::optional<SHA256Hash>
  get_post_block_deployment(ContractAddress const &addr);
  std::shared_ptr<const Contract>
  get_post_block_contract(SHA256Hash const &hash);

  bool is_closed() const { return closed_; }

protected:
  ParentBlockView(InMemoryDatabase *root_db) : parent_view_(root_db) {}
  ParentBlockView(ParentBlockView *parent_view) : parent_view_(parent_view) {}

  // the value of the item, before any modifications of the current block are
  // applied. Should not be called after the block is closed.
  std::shared_ptr<const StorageItemWrapper>
  get_snapshot_storage_item(AddressAndKey const &addrkey);

  // Methods relevant for contracts
  // TODO(geoff): is there a cleaner way to manage this than
  // just method duplication?
  // Read from snapshot data, not data created by concurrent operations.
  // Should not be called after the block is closed.
  std::optional<SHA256Hash>
  get_snapshot_deployment(ContractAddress const &addr);
  std::shared_ptr<const Contract> get_snapshot_contract(SHA256Hash const &hash);

  // Lookup committed item from parent StorageView or from parent database.
  std::shared_ptr<const StorageItemWrapper>
  get_parent_storage_item(AddressAndKey const &addrkey);

  SynchronizedMap<AddressAndKey, StorageItemDeltaManager> modified_items_;
  InvalidStateFlag invalid_state_flag_;
  bool closed_ = false;

  // simple impl, for now
  x23::MVar<std::unordered_map<ContractAddress, SHA256Hash>>
      contract_deployments_;
  x23::MVar<std::unordered_map<SHA256Hash, std::shared_ptr<const Contract>>>
      contract_creates_;

  // Does NOT own the parent StorageView.
  // The ParentBlockView is owned by BlockState and might be freed at any time
  // (after unlink_parent is called).  As such, the ParentBlockView pointer
  // should never be exposed outside of this interface. It is always accessed
  // while mtx_ is held (in shared mode). unlink_parent acquires the mutex in
  // exclusive mode, so by the time unlink_parent returns, any pending accesses
  // to the parent_view_ have finished.
  std::shared_mutex mtx_;
  std::variant<ParentBlockView *, InMemoryDatabase *> parent_view_;
};

// A StorageView extends a ParentBlockView with methods to modify the database.
class StorageView : private ParentBlockView {
public:
  template<typename... Args>
  StorageView(Args... args) : ParentBlockView(std::forward<Args>(args)...)
  {}

  // TODO(geoff): This isn't strictly necessary, but useful sanity check
  void close_block() { closed_ = true; }
  void assert_block_closed() { assert(closed_); }
  void assert_block_not_closed() { assert(!closed_); }

  // parent must not be another storage view (i.e. call unlink_parent first).
  void commit_to_database();

  // Parent view has been committed to the persistent database.
  // Rather than jump through the parent StorageView,
  // at that point, we can just directly access the committed database.
  // TODO(geoff): There will be multiple kinds of backing databases.
  void unlink_parent(InMemoryDatabase *new_parent);

  ParentBlockView *as_parent()
  {
    assert(closed_);
    return this;
  }

  std::optional<MeteredStorageLock>
  try_reserve_storage_delta(AddressAndKey const &addrkey,
                            StorageDelta const &delta, StorageLock const &lock,
                            StorageItemRPCFlagSet flags);
  void revert_storage_lock(AddressAndKey const &addrkey,
                           StorageLock const &lock);
  int128_t commit_storage_delta(AddressAndKey const &addrkey,
                                StorageDelta const &delta,
                                MeteredStorageLock const &lock);

  void commit_contract_create(Contract const &contract);
  void commit_contract_deploy(ContractAddress const &addr,
                              SHA256Hash const &contract_hash);

  bool has_bad_item() const { return invalid_state_flag_.is_invalid(); }

  using ParentBlockView::get_snapshot_contract;
  using ParentBlockView::get_snapshot_deployment;
  using ParentBlockView::get_snapshot_storage_item;
  using ParentBlockView::is_closed;
};

} // namespace hogd
