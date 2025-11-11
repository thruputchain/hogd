#include "hogd/hogdb/item_lock/auth_lock.h"

#include <cassert>
#include <mutex>
#include <span>

#include "account.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/merkle.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "x23/serialize.h"
#include "x23/testlock.h"

namespace hogd {

AccountAuthData
AuthLock::build_result() const
{
  AccountAuthData result = base_auth_data_;
  for (size_t i = 0; i < MAX_SIGNERS; ++i) {
    if (pk_changes_[i].has_value()) {
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      result.public_keys[i] = pk_changes_[i].value();
    }
  }
  return result;
}

size_t
AuthLock::count_active_keys() const
{
  size_t active_keys = 0;
  for (size_t i = 0; i < MAX_SIGNERS; ++i) {
    if (pk_changes_[i].has_value()) {
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      if (pk_changes_[i].value()) {
        active_keys++;
      }
    }
    else {
      if (base_auth_data_.public_keys[i]) {
        active_keys++;
      }
    }
  }
  return active_keys;
}

HogDBStatus<int128_t>
AuthLock::try_atomic_change_lock(const PointerItem<AuthDataItem> *before,
                                 const PointerItem<AuthDataItem> *after,
                                 const StorageCostModel *cost_model)
{

  std::unique_lock<x23::TestLock> ul(lock_);
  if (locked_ && before == nullptr) {
    return HogDBError{HogDBCode::kConflict};
  }
  assert(before == nullptr ||
         before->get()->base_auth_data() == base_auth_data_);

  if (after != nullptr) {
    if (after->get()->base_auth_data() != base_auth_data_) {
      return HogDBError{HogDBCode::kConflict};
    }
    locked_ = true;
  }
  else {
    locked_ = false;
  }

  size_t keys = 0;
  // count number of keys created by after
  for (size_t i = 0; i < MAX_SIGNERS; ++i) {
    if (after != nullptr && after->get()->pk_changes_[i].has_value()) {
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      auto const &change = after->get()->pk_changes_[i].value();
      if (change || base_auth_data_.public_keys[i]) {
        keys++;
      }
    }
    else {
      if (base_auth_data_.public_keys[i]) {
        keys++;
      }
    }
  }

  return cost_model->get_auth_data_cost(keys);
}

int128_t
AuthLock::commit_change(bool did_finalize, int128_t snapshot_cost,
                        const PointerItem<AuthDataItem> &after,
                        const StorageCostModel *cost_model)
{
  std::unique_lock<x23::TestLock> ul(lock_);
  CHECK(locked_);

  const AuthDataItem &after_item = *after.get();

  // It's this kind of logic that prevents us from pulling the
  // did_finalize/snapshot logic out of the type-specific locks and into the
  // base ItemLock.
  int128_t previous_cost =
      did_finalize ? snapshot_cost
                   : cost_model->get_auth_data_cost(count_active_keys());

  for (size_t i = 0; i < MAX_SIGNERS; ++i) {

    if (after_item.pk_changes_[i].has_value()) {
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      auto const &change = after_item.pk_changes_[i].value();
      pk_changes_[i] = change;
    }
  }

  int128_t new_cost = cost_model->get_auth_data_cost(count_active_keys());
  return new_cost - previous_cost;
}

HogDBStatus<void>
AuthLock::write_committed_change(ItemKey const &key, HogDBDAO &dao,
                                 MerkleTriePrefix &trie) const
{
  assert(key.is_account_auth_data());

  auto res = build_result();
  auto ret = dao.write_address_account_auth_data(key.get_address(), res);
  if (!ret) {
    return ret.error();
  }

  auto buf = x23::serialize(res);
  CHECK(MerkleTrieHelpers::update_kv_pair_in_trie(trie, key, 0,
                                                  std::as_bytes(std::span(buf)))
            .has_value());
  return ret;
}

} // namespace hogd
