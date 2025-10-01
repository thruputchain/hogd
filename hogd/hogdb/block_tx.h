// block_tx.h - transaction handling logic at the granularity of a block.

#pragma once

#include <mutex>

#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

// BlockTX manages a set of pending changes made in a block relative to the
// current snapshot state. They can be applied to the ramdb via commit().
// Multiple BlockTXes can be instantiated, but only one should be committed.
class BlockTX {
public:
  BlockTX(HogDBDAO &db, MerkleTriePrefix &trie,
          const StorageCostModel &cost_model)
    : db_(db), trie_(trie), cost_model_(cost_model)
  {}
  ~BlockTX() = default;

  // getters
  HogDBDAO &get_db() const { return db_; }
  MerkleTriePrefix &get_trie() const { return trie_; }

  // commit applies all changes to the database. If this fails, nothing will be
  // written to disk, but the database may be left in an inconsistent state.
  HogDBStatus<void> commit(WorkerPool &pool = WorkerPool::get_default_pool());
  // abort drops all changes.
  void abort()
  {
    addr_balance_locks_.clear();
    item_locks_.clear();
  }

  // lock reserves the right to apply change to an item.
  // Lock returns the amount of storage tokens at least as large as what will
  // be needed to support the change, if committed.
  HogDBStatus<int128_t> lock(const ItemKey &key, ItemChange &change);
  // modify_lock modifies an existing change to an item that was reserved with
  // lock(). It could still fail if new_change has a conflict.
  // It returns an amount of storage tokens at least as large as what will
  // be needed to support the change, if committed.
  HogDBStatus<int128_t> modify_lock(const ItemKey &key, ItemChange &old_change,
                                    ItemChange &new_change);
  // unlock unreserves the right to apply change to an item and reverts it.
  void unlock(const ItemKey &key, ItemChange &change);
  // commit_change marks a change as committed.
  // returns the increase in committed storage cost.
  int128_t commit_change(const ItemKey &key, ItemChange const &change);

  void release_balance(const Address &addr, int128_t delta);

private:
  struct ItemLock {
    ItemChange change;
    size_t ref;
  };

  HogDBDAO &db_;
  MerkleTriePrefix &trie_;
  StorageCostModel cost_model_;
  SynchronizedMap<Address, ItemLock> addr_balance_locks_;
  SynchronizedMap<ItemKey, ItemLock> item_locks_;

  // raw locking operations.
  static inline HogDBStatus<int128_t>
  do_lock(const std::unique_lock<std::mutex> &ul, ItemLock &lock,
          const ItemChange &change, const StorageCostModel *cost_model);
  static inline HogDBStatus<int128_t>
  do_modify_lock(const std::unique_lock<std::mutex> &ul, ItemLock &lock,
                 const ItemChange &old_change, const ItemChange &new_change,
                 const StorageCostModel *cost_model);
  void inline do_unlock_item(const std::unique_lock<std::mutex> &ul,
                             const ItemKey &key, ItemLock &lock,
                             const ItemChange &change);
  void inline do_unlock_addr_balance(const std::unique_lock<std::mutex> &ul,
                                     const Address &addr, ItemLock &lock,
                                     const ItemChange &change);

  // balance operations for charging for storage.
  HogDBStatus<void> lock_balance(const Address &addr, int128_t delta);
  // HogDBStatus<void> modify_balance(const Address &addr, int128_t old_delta,
  //                                  int128_t new_delta);
};

} // namespace hogd
