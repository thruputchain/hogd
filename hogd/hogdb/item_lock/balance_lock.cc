#include "hogd/hogdb/item_lock/balance_lock.h"

#include <cassert>
#include <cstddef>

#include "db_sched.hh"
#include "hogd/base/bits.h"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogdb/change_set.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {

HogDBStatus<int128_t>
BalanceLock::try_atomic_change_lock(const Balance *before, const Balance *after,
                                    const LockContext &ctx)
{

  int128_t add_before = before == nullptr ? 0 : before->plus();
  int128_t sub_before = before == nullptr ? 0 : before->minus();
  int128_t add_after = after == nullptr ? 0 : after->plus();
  int128_t sub_after = after == nullptr ? 0 : after->minus();
  int128_t delta_before = before == nullptr ? 0 : before->delta();
  int128_t delta_after = after == nullptr ? 0 : after->delta();

  assert(ctx.key.is_address_balance());

  // check if we need to guard for storage costs of the balance item
  auto snapshot_value = snapshot_balance_.snapshot_value();
  // If the item exists in the snapshot, then it has already been committed in a
  // previous block and thus already factors in its own storage cost.
  if (!snapshot_value.has_value()) {
    // It must be the case, in this scenario, that the balance delta is positive
    // and sufficiently positive to cover the storage cost of the balance item.
    // Or after is nullptr, in which case we are undoing a change and need not
    // worry about this.
    if (after != nullptr) {
      int128_t delta_after = after->delta();
      if (delta_after < get_storage_cost_of_committed_value(ctx.cost_model)) {
        return HogDBError{HogDBCode::kConflict};
      }
    }
  }

  if (!delta_bounds_.try_reserve_delta(delta_before, delta_after, add_before,
                                       add_after, sub_before, sub_after)) {
    return HogDBError{HogDBCode::kConflict};
  }

  return 0;
}

int128_t
BalanceLock::build_result(LockContext const &ctx) const
{
  auto snapshot_value_opt = snapshot_balance_.snapshot_value();
  int128_t snapshot_value =
      snapshot_value_opt.has_value() ? *snapshot_value_opt : 0;

  auto delta = delta_bounds_.get_delta();
  auto [value, of] = overflowing_add(snapshot_value, delta);
  CHECK(!of);

  if (!snapshot_value_opt.has_value()) {
    // We must, in this case, additionally cover the storage cost of the balance
    // item.
    value -= get_storage_cost_of_committed_value(ctx.cost_model);
    CHECK(value >= 0);
  }
  return value;
}

HogDBStatus<void>
BalanceLock::stage_committed_changes(const LockContext &ctx,
                                     ChangeSet &changes) const
{
  CHECK(ctx.key.is_address_balance());

  auto value = build_result(ctx);
  CHECK_GE(value, 0);

  // Create the HogAge struct when the balance entry is created for the first
  // time (not part of blockchain state — ChangeSet will skip hash element
  // construction because is_state_item returns false for kHogAge).
  if (!snapshot_balance_.snapshot_value().has_value()) {
    uint128_t value_uint128 = value;
    HogAge age = {
        .address = ctx.key.to_contract_address(),
        .num_hogs = {to_xdr(value_uint128), to_xdr(uint128_t{0}),
                     to_xdr(uint128_t{0})},
        .average_ages = {BlockHeight{0}, BlockHeight{0}, BlockHeight{0}},
    };
    auto hog_age_key = make_address_key(ctx.key.get_address(), Prefix::kHogAge);
    auto age_buf = x23::serialize(age);
    changes.put(hog_age_key, 0, age_buf);
  }

  auto encoded = int128_to_xdr_bytes(value);
  auto key = make_address_key(ctx.key.get_address(), Prefix::kBalance);
  changes.put(key, kNNInt128, encoded);
  return {};
}

} // namespace hogd
