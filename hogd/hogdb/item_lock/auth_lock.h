#pragma once

#include <array>
#include <optional>
#include <utility>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/pointer_lock.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "x23/testlock.h"
#include "x23/vec.h"

namespace hogd {

// TODO(geoff): This version of the auth lock does not implement multiple
// transactions modifying the auth data in one block.  We can, and probably
// should, implement that eventually. However, before we lock down what the
// precise format of the auth data is, and how pieces of it other than the
// public keys are modified, I'm leaving this as the simple implementation.
// Getting the concurrent version right requires sufficiently complex, bugprone
// code that is difficult to change. Once we finalize what are all the ways to
// modify the auth data and the auth data's exact format, we can revisit this to
// implement a concurrent version.
class AuthLock {
public:
  using change_type = PointerItem<AuthDataItem>;

  AuthLock(AddressAuthData base_auth_data)
    : base_auth_data_(std::move(base_auth_data))
  {}

  HogDBStatus<int128_t>
  try_atomic_change_lock(const PointerItem<AuthDataItem> *before,
                         const PointerItem<AuthDataItem> *after,
                         const LockContext &ctx);
  int128_t commit_change(bool did_finalize, int128_t snapshot_cost,
                         PointerItem<AuthDataItem> &after,
                         const LockContext &ctx);
  HogDBStatus<void> stage_committed_changes(const LockContext &ctx,
                                            ChangeSet &changes) const;

  AddressAuthData build_result() const;

private:
  size_t count_active_keys() const;

  AddressAuthData base_auth_data_;
  x23::TestLock lock_;
  bool locked_{false};
  std::array<std::optional<PublicKeyConfigOpt>, kMaxSigners> pk_changes_;
  std::optional<Weight> required_weight_change_;

  friend class AuthDataItem;
};

static_assert(item_lock_unsized<AuthLock>);
static_assert(item_lock<PointerLock<AuthLock>>);

} // namespace hogd
