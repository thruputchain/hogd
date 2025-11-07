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
    auto const &pk_change = pk_changes_[i];
    if (pk_change.committed_) {
      result.public_keys[i] = pk_change.pk_;
    }
  }
  return result;
}

size_t
AuthLock::count_active_keys() const
{
  size_t active_keys = 0;
  for (size_t i = 0; i < MAX_SIGNERS; ++i) {
    if (pk_changes_[i].committed_) {
      if (pk_changes_[i].pk_) {
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

  const AuthDataItem *before_item = before ? before->get() : nullptr;
  const AuthDataItem *after_item = after ? after->get() : nullptr;

  assert(before_item == nullptr ||
         before_item->base_auth_data() == base_auth_data_);
  if (after_item != nullptr &&
      after_item->base_auth_data() != base_auth_data_) {
    return HogDBError{HogDBCode::kConflict};
  }

  // First pass: check for conflicts (must be done before modifying state)
  for (size_t i = 0; i < MAX_SIGNERS; ++i) {
    bool before_modifies = ((before_item != nullptr) &&
                            (before_item->pk_changes_[i].refcount_ > 0));
    bool after_modifies =
        ((after_item != nullptr) && (after_item->pk_changes_[i].refcount_ > 0));
    bool before_exclusive =
        ((before_item != nullptr) && (before_item->pk_changes_[i].exclusive_));
    bool after_exclusive =
        ((after_item != nullptr) && (after_item->pk_changes_[i].exclusive_));
    assert(before_item == nullptr ||
           (before_item->pk_changes_[i].refcount_ <= 1));
    assert(after_item == nullptr ||
           (after_item->pk_changes_[i].refcount_ <= 1));

    // after_exclusive implies after_modifies
    // before_exclusive implies before_modifies
    assert(!after_exclusive || after_modifies);
    assert(!before_exclusive || before_modifies);

    // !before_modifies  && !after_modifies: nothing to do

    // If acquiring a new lock, check for conflicts
    if (!before_modifies && after_modifies) {
      // If trying to acquire an exclusive lock, no one else should hold it
      if (after_exclusive && pk_changes_[i].refcount_ > 0) {
        return HogDBError{HogDBCode::kConflict};
      }
      // If someone else holds an exclusive lock, conflict
      if (pk_changes_[i].exclusive_) {
        return HogDBError{HogDBCode::kConflict};
      }
      // If the pk values don't match existing locks, conflict
      // We don't already have a reference
      if (pk_changes_[i].refcount_ > 0 &&
          after_item->pk_changes_[i].pk_ != pk_changes_[i].pk_) {
        return HogDBError{HogDBCode::kConflict};
      }
    }

    // before_modifies && !after_modifies: safe to downgrade from refcount = 1
    // to refcount = 0

    // If upgrading from non-exclusive to exclusive
    if (before_modifies && after_modifies && !before_exclusive &&
        after_exclusive) {
      // We already hold one lock, so no one else should hold any
      if (pk_changes_[i].refcount_ != 1) {
        return HogDBError{HogDBCode::kConflict};
      }
      // The pk values should match
      assert(before_item->pk_changes_[i].pk_ == pk_changes_[i].pk_);
    }

    // safe to downgrade from exclusive to non-exclusive, or from refcount = 1
    // to refcount = 0
    if (before_modifies && after_modifies && !before_exclusive &&
        !after_exclusive) {
      assert(before_item->pk_changes_[i].pk_ == pk_changes_[i].pk_);
      // only safe to change the pk without exclusive lock if refcount == 1
      if (pk_changes_[i].refcount_ != 1 &&
          after_item->pk_changes_[i].pk_ != pk_changes_[i].pk_) {
        return HogDBError{HogDBCode::kConflict};
      }
    }
  }

  // Second pass: apply changes (now that we know there are no conflicts)
  for (size_t i = 0; i < MAX_SIGNERS; ++i) {
    bool before_modifies = ((before_item != nullptr) &&
                            (before_item->pk_changes_[i].refcount_ > 0));
    bool after_modifies =
        ((after_item != nullptr) && (after_item->pk_changes_[i].refcount_ > 0));
    bool before_exclusive =
        ((before_item != nullptr) && (before_item->pk_changes_[i].exclusive_));
    bool after_exclusive =
        ((after_item != nullptr) && (after_item->pk_changes_[i].exclusive_));

    // Release lock
    if (before_modifies && !after_modifies) {
      assert(pk_changes_[i].refcount_ > 0);
      pk_changes_[i].refcount_--;
      // Release exclusive flag if we held it
      if (before_exclusive) {
        assert(pk_changes_[i].exclusive_);
        assert(pk_changes_[i].refcount_ == 0); // We just decremented to 0
        pk_changes_[i].exclusive_ = false;
      }
    }
    // Acquire new lock
    else if (!before_modifies && after_modifies) {
      pk_changes_[i].refcount_++;
      pk_changes_[i].pk_ = after_item->pk_changes_[i].pk_;
      if (after_exclusive) {
        pk_changes_[i].exclusive_ = true;
      }
    }
    // Upgrade to exclusive (keep lock, add exclusive flag)
    else if (before_modifies && after_modifies && !before_exclusive &&
             after_exclusive) {
      assert(pk_changes_[i].refcount_ == 1);
      pk_changes_[i].exclusive_ = true;
    }
    // Downgrade from exclusive (keep lock, remove exclusive flag)
    else if (before_modifies && after_modifies && before_exclusive &&
             !after_exclusive) {
      assert(pk_changes_[i].exclusive_);
      pk_changes_[i].exclusive_ = false;
    }
    // else: keep lock with same state - no changes needed
  }

  // Return upper bound on storage cost
  size_t max_keys = 0;
  for (size_t i = 0; i < MAX_SIGNERS; ++i) {
    // The returned storage cost needs to be a deterministic function of
    // base_auth_data_ and after_item. We also can't release storage costs until
    // commit time. As such, a key is charged if: (1) it exists in the base auth
    // data (2) it is possibly created by this tx
    bool possibly_created = (after_item != nullptr) &&
                            ((after_item->pk_changes_[i].refcount_ > 0) &&
                             (after_item->pk_changes_[i].pk_ != nullptr));
    if (possibly_created || base_auth_data_.public_keys[i]) {
      max_keys++;
    }
  }
  return cost_model->get_auth_data_cost(max_keys);
}

int128_t
AuthLock::commit_change(bool did_finalize, int128_t snapshot_cost,
                        const PointerItem<AuthDataItem> &after,
                        const StorageCostModel *cost_model)
{
  std::unique_lock<x23::TestLock> ul(lock_);

  const AuthDataItem &after_item = *after.get();

  // It's this kind of logic that prevents us from pulling the
  // did_finalize/snapshot logic out of the type-specific locks and into the
  // base ItemLock.
  int128_t previous_cost =
      did_finalize ? snapshot_cost
                   : cost_model->get_auth_data_cost(count_active_keys());

  // Mark changes as committed and clear exclusive flags
  // refcount_ in AuthDataItem is 0 or 1, indicating if this tx touches that key
  for (size_t i = 0; i < MAX_SIGNERS; ++i) {
    bool modifies = (after_item.pk_changes_[i].refcount_ > 0);

    if (modifies) {
      assert(pk_changes_[i].refcount_ > 0);
      assert(after_item.pk_changes_[i].pk_ == pk_changes_[i].pk_);
      // Mark this key's change as committed
      pk_changes_[i].committed_ = true;

      // the exclusive flag is set if and only if the change committing has it
      assert(after_item.pk_changes_[i].exclusive_ == pk_changes_[i].exclusive_);

      // Clear exclusive flag when a change is committed
      if (after_item.pk_changes_[i].exclusive_) {
        assert(after_item.pk_changes_[i].refcount_ == 1);
        pk_changes_[i].exclusive_ = false;
      }
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
