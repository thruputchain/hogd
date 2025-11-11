// block_tx.h - transaction handling logic at the granularity of a block.

#pragma once

#include <memory>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/concurrent_map.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "sched_db.hh"
#include "types.hh"
#include "x23/rpc.h"

namespace hogd {

// BlockTX manages a set of pending changes made in a block relative to the
// current snapshot state. They can be applied to the ramdb via commit().
// Multiple BlockTXes can be instantiated, but only one should be committed.
class BlockTX {
public:
  using sched_db_client_t =
      SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

  BlockTX(HogDBDAO &db, MerkleTriePrefix &trie,
          const StorageCostModel &cost_model,
          std::shared_ptr<sched_db_client_t> sched_db_client,
          BlockHeight block_height)
    : db_(db), trie_(trie), cost_model_(cost_model),
      sched_db_client_(std::move(sched_db_client)), block_height_(block_height)
  {}
  ~BlockTX() = default;

  // getters
  HogDBDAO &get_db() const { return db_; }
  MerkleTriePrefix &get_trie() const { return trie_; }
  BlockHeight get_height() const { return block_height_; }

  // commit applies all changes to the database. If this fails, nothing will be
  // written to disk, but the database may be left in an inconsistent state.
  HogDBStatus<void> commit(WorkerPool &pool = WorkerPool::get_default_pool());

  // lock reserves the right to apply change to an item.
  // Lock returns the amount of storage tokens at least as large as what will
  // be needed to support the change, if committed.
  HogDBStatus<int128_t> lock(const ItemKey &key, ItemChange &change);
  // modify_lock modifies an existing change to an item that was reserved with
  // lock(). It could still fail if new_change has a conflict.
  // It returns an amount of storage tokens at least as large as what will
  // be needed to support the change, if committed.
  HogDBStatus<int128_t> modify_lock(const ItemKey &key,
                                    const ItemChange &old_change,
                                    const ItemChange &new_change);
  // unlock unreserves the right to apply change to an item and reverts it.
  void unlock(const ItemKey &key, ItemChange &change);
  // commit_change marks a change as committed.
  // returns the increase in committed storage cost.
  int128_t commit_change(const ItemKey &key, ItemChange const &change);

  void release_balance(const Address &addr, int128_t delta);

private:
  HogDBStatus<int128_t> get_snapshot_cost(const ItemKey &key);

  struct SipHash24Hasher {
    size_t operator()(const Address &key) const
    {
      static SipHash24 hasher;
      return hasher(key.as_bytes());
    }
    size_t operator()(const ItemKey &key) const
    {
      static SipHash24 hasher;
      return hasher(key.as_bytes());
    }
  };

  HogDBDAO &db_;
  MerkleTriePrefix &trie_;
  StorageCostModel cost_model_;
  ConcurrentMap<Address, ItemLock, SipHash24Hasher> addr_balance_locks_;
  ConcurrentMap<ItemKey, ItemLock, SipHash24Hasher> item_locks_;
  std::shared_ptr<sched_db_client_t> sched_db_client_;
  BlockHeight block_height_;

  static inline HogDBStatus<int128_t>
  do_lock(ItemLock &lock, const ItemChange &change,
          const StorageCostModel *cost_model);
  static inline HogDBStatus<int128_t>
  do_modify_lock(ItemLock &lock, const ItemChange &old_change,
                 const ItemChange &new_change,
                 const StorageCostModel *cost_model);
  static inline void do_unlock_item(ItemLock &lock, const ItemChange &change,
                                    const StorageCostModel *cost_model);
};

} // namespace hogd
