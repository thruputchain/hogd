// block_tx.h - transaction handling logic at the granularity of a block.

#pragma once

#include <utility>

#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"

namespace hogd {

// BlockTX manages a set of pending changes made in a block relative to the
// current snapshot state. They can be applied to the ramdb via commit().
// Multiple BlockTXes can be instantiated, but only one should be committed.
class BlockTX {
public:
  BlockTX(HogDatabase &db, MerkleTriePrefix &trie) : db_(db), trie_(trie) {}
  ~BlockTX() = default;

  // getters
  HogDatabase &get_db() const { return db_; }
  MerkleTriePrefix &get_trie() const { return trie_; }

  // commit applies all changes to the database. If this fails, nothing will be
  // written to disk, but the database may be left in an inconsistent state.
  HogDBStatus<void> commit();
  // abort drops all changes.
  void abort()
  {
    addr_balance_locks_.clear();
    item_locks_.clear();
  }

  // lock reserves the right to apply change to an item.
  HogDBStatus<void> lock(const ItemKey &key, ItemChange &change);
  // modify_lock modifies an existing change to an item that was reserved with
  // lock(). It could still fail if new_change has a conflict.
  HogDBStatus<void> modify_lock(const ItemKey &key, ItemChange &old_change,
                                ItemChange &new_change);
  // unlock unreserves the right to apply change to an item and reverts it.
  void unlock(const ItemKey &key, ItemChange &change);

private:
  struct ItemLock {
    ItemChange change;
    size_t ref;
  };

  HogDatabase &db_;
  MerkleTriePrefix &trie_;
  SynchronizedMap<Address, ItemLock> addr_balance_locks_;
  SynchronizedMap<ItemKey, ItemLock> item_locks_;

  // raw locking operations.
  static inline HogDBStatus<void>
  do_lock(const std::unique_lock<std::mutex> &ul, ItemLock &lock,
          const ItemChange &change);
  static inline HogDBStatus<void>
  do_modify_lock(const std::unique_lock<std::mutex> &ul, ItemLock &lock,
                 const ItemChange &old_change, const ItemChange &new_change);
  void inline do_unlock_item(const std::unique_lock<std::mutex> &ul,
                             const ItemKey &key, ItemLock &lock,
                             const ItemChange &change);
  void inline do_unlock_addr_balance(const std::unique_lock<std::mutex> &ul,
                                     const Address &addr, ItemLock &lock,
                                     const ItemChange &change);

  // balance operations for charging for storage.
  HogDBStatus<void> lock_balance(const Address &addr, int128_t delta);
  HogDBStatus<void> modify_balance(const Address &addr, int128_t old_delta,
                                   int128_t new_delta);
  void unlock_balance(const Address &addr, int128_t delta);
};

} // namespace hogd
