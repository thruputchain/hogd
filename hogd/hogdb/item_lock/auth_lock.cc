#include "hogd/hogdb/item_lock/auth_lock.h"

#include <cassert>
#include <mutex>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "db_conflict.hh"
#include "types.hh"

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/change_set.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "x23/serialize.h"
#include "x23/testlock.h"

namespace hogd {

AddressAuthData
AuthLock::build_result(BlockHeight block_height) const
{
  AddressAuthData result = base_auth_data_;
  for (size_t i = 0; i < kMaxSigners; ++i) {
    if (pk_changes_[i].has_value()) {
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      result.public_keys[i] = pk_changes_[i].value();
    }
  }
  if (required_weight_change_.has_value()) {
    result.required_weight = required_weight_change_.value();
  }
  // Stamp the row with the height of the committing block so readers can detect
  // a row written by a block newer than the height their query was accepted
  // for: during scheduler commit the new auth row is applied to the live DAO
  // before the committed tip advances, so an unprotected read can return
  // next-height auth data (issue #1725). build_result is the single point that
  // both the DB-write path (stage_committed_changes) and the auth-subscriber
  // notification path (BlockTX::commit) flow through, so stamping here keeps
  // the two copies consistent.
  result.last_written_height = block_height;
  return result;
}

size_t
AuthLock::count_active_keys() const
{
  size_t active_keys = 0;
  for (size_t i = 0; i < kMaxSigners; ++i) {
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
                                 const LockContext &ctx)
{

  std::unique_lock<x23::TestLock> ul(lock_);
  if (locked_ && before == nullptr) {
    return ctx.conflict(DbConflictCode::kDbConflictExclusiveLock);
  }
  assert(before == nullptr ||
         before->get()->base_auth_data() == base_auth_data_);

  if (after != nullptr) {
    if (after->get()->base_auth_data() != base_auth_data_) {
      return ctx.conflict(DbConflictCode::kDbConflictValueMismatch);
    }
    locked_ = true;
  }
  else {
    locked_ = false;
  }

  size_t keys = 0;
  // count number of keys created by after
  for (size_t i = 0; i < kMaxSigners; ++i) {
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

  return ctx.cost_model->get_auth_data_cost(keys);
}

int128_t
AuthLock::commit_change(bool did_finalize, int128_t snapshot_cost,
                        PointerItem<AuthDataItem> &after,
                        const LockContext &ctx)
{
  std::unique_lock<x23::TestLock> ul(lock_);
  CHECK(locked_);

  const AuthDataItem &after_item = *after.get();

  // It's this kind of logic that prevents us from pulling the
  // did_finalize/snapshot logic out of the type-specific locks and into the
  // base ItemLock.
  int128_t previous_cost =
      did_finalize ? snapshot_cost
                   : ctx.cost_model->get_auth_data_cost(count_active_keys());

  for (size_t i = 0; i < kMaxSigners; ++i) {

    if (after_item.pk_changes_[i].has_value()) {
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      auto const &change = after_item.pk_changes_[i].value();
      pk_changes_[i] = change;
    }
  }

  if (after_item.required_weight_change_.has_value()) {
    required_weight_change_ = after_item.required_weight_change_;
  }
  int128_t new_cost = ctx.cost_model->get_auth_data_cost(count_active_keys());
  return new_cost - previous_cost;
}

HogDBStatus<void>
AuthLock::stage_committed_changes(const LockContext &ctx,
                                  ChangeSet &changes) const
{
  CHECK(ctx.key.is_address_auth_data());
  auto auth = build_result(ctx.block_height);
  auto key = make_address_key(ctx.key.get_address(), Prefix::kAddressAuthData);
  auto serialized = x23::serialize(auth);
  changes.put(key, 0, serialized);
  return {};
}

} // namespace hogd
