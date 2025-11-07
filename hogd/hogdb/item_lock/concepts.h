#pragma once

#include <concepts>

#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

template<typename Lock>
concept item_lock_unsized =
    requires { typename Lock::change_type; } &&
    requires(Lock lock, const Lock::change_type *before,
             const Lock::change_type *after, const StorageCostModel *cost_model,
             bool finalize, int128_t snapshot_cost) {
      {
        lock.try_atomic_change_lock(before, after, cost_model)
      } -> std::same_as<HogDBStatus<int128_t>>;
      {
        lock.commit_change(finalize, snapshot_cost, *after, cost_model)
      } -> std::same_as<int128_t>;
    } &&
    requires(Lock const &lock, ItemKey const &key, HogDBDAO &dao,
             MerkleTriePrefix &trie) {
      {
        lock.write_committed_change(key, dao, trie)
      } -> std::same_as<HogDBStatus<void>>;
    };

template<typename Lock>
concept item_lock = item_lock_unsized<Lock> && requires { sizeof(Lock) <= 64; };
} // namespace hogd
