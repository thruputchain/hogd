#pragma once

#include <array>
#include <optional>
#include <utility>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "hogd/base/types.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/item_lock/concepts.h"
#include "hogd/hog_db/item_lock/pointer_lock.h"
#include "hogd/hog_db/status.h"
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

  // A lock on an address whose committed row is base_auth_data.
  AuthLock(AddressAuthData base_auth_data)
    : AuthLock(std::move(base_auth_data), /*row_exists=*/true)
  {}
  AuthLock(AddressAuthData base_auth_data, bool row_exists)
    : base_auth_data_(std::move(base_auth_data)), row_exists_(row_exists)
  {}

  HogDbStatus<int128_t>
  try_atomic_change_lock(const PointerItem<AuthDataItem> *before,
                         const PointerItem<AuthDataItem> *after,
                         const LockContext &ctx);
  int128_t commit_change(bool did_finalize, int128_t snapshot_cost,
                         PointerItem<AuthDataItem> &after,
                         const LockContext &ctx);
  HogDbStatus<void> stage_committed_changes(const LockContext &ctx,
                                            ChangeSet &changes) const;

  AddressAuthData build_result(BlockHeight block_height) const;

  // Whether the committed changes leave a row to write. An address with no
  // committed row gets one only once a change registers a key or writes a
  // threshold; until then the lock is held, but nothing is written or charged.
  bool writes_row() const;

private:
  size_t count_active_keys() const;
  int128_t committed_cost(const StorageCostModel &cost_model) const;

  // Whether [after] registers a key in a slot while another slot of the
  // table it produces (the committed row with [after] applied) holds the
  // same key. Such a table would count one key's weight twice, so
  // try_atomic_change_lock refuses the change with DuplicateSigner. Only
  // slots [after] sets are checked: a duplicate already committed does not
  // block changes that leave it alone, and a change that clears one copy
  // while keeping the other is a move, not a duplicate.
  bool introduces_duplicate_signer(AuthDataItem const &after) const;

  AddressAuthData base_auth_data_;
  bool row_exists_;
  x23::TestLock lock_;
  bool locked_{false};
  std::array<std::optional<PublicKeyConfigOpt>, kMaxSigners> pk_changes_;
  std::optional<Weight> required_weight_change_;

  friend class AuthDataItem;
};

static_assert(item_lock_unsized<AuthLock>);
static_assert(item_lock<PointerLock<AuthLock>>);

} // namespace hogd
