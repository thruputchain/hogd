#pragma once

#include <array>
#include <utility>

#include "account.hh"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/pointer_lock.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "x23/testlock.h"

namespace hogd {

class AuthLock {
public:
  using change_type = PointerItem<AuthDataItem>;

  AuthLock(AccountAuthData base_auth_data)
    : base_auth_data_(std::move(base_auth_data))
  {}

  HogDBStatus<int128_t>
  try_atomic_change_lock(const PointerItem<AuthDataItem> *before,
                         const PointerItem<AuthDataItem> *after,
                         const StorageCostModel *cost_model);
  int128_t commit_change(bool did_finalize, int128_t snapshot_cost,
                         const PointerItem<AuthDataItem> &after,
                         const StorageCostModel *cost_model);
  HogDBStatus<void> write_committed_change(ItemKey const &key, HogDBDAO &dao,
                                           MerkleTriePrefix &trie) const;

private:
  struct PublicKeyChange {
    PublicKeyConfigOpt pk_ = nullptr;
    uint32_t refcount_{0};
    bool exclusive_{false};
    bool committed_{false};
  };

  AccountAuthData build_result() const;
  size_t count_active_keys() const;

  AccountAuthData base_auth_data_;
  std::array<PublicKeyChange, MAX_SIGNERS> pk_changes_;
  x23::TestLock lock_;

  friend class AuthDataItem;
};

static_assert(item_lock_unsized<AuthLock>);
static_assert(item_lock<PointerLock<AuthLock>>);

} // namespace hogd
