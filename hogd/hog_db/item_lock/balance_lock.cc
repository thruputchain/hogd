#include "hogd/hog_db/item_lock/balance_lock.h"

#include <cassert>
#include <cstddef>

#include "db_conflict.hh"
#include "db_sched.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hog_db/change_set.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/item_lock/concepts.h"
#include "hogd/hog_db/status.h"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {

HogDbStatus<int128_t>
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
        return ctx.conflict(DbConflictCode::kDbConflictBoundsExceeded);
      }
    }
  }

  // A balance's committed value is snapshot + delta and must stay within
  // [0, INT128_MAX]. Enforce that domain here, at the lock entrypoint, using
  // the snapshot the lock already holds (no extra base read). Unlike NNInt128
  // deltas, balance deltas are otherwise unbounded -- Balance::narrow performs
  // no base check -- so a narrowed opposite-sign reservation could leave
  // delta_after outside this range and make delta_after - delta_before overflow
  // inside NNInt128DeltaBounds::try_reserve_delta, tripping its assert.
  // Bounding delta_after to [-snapshot, INT128_MAX - snapshot] keeps that
  // subtraction in the same non-overflowing domain NNInt128 relies on.
  int128_t snapshot = snapshot_value.value_or(0);
  auto [committed, committed_overflow] = overflowing_add(snapshot, delta_after);
  if (committed_overflow || committed < 0) {
    return ctx.conflict(DbConflictCode::kDbConflictBoundsExceeded);
  }

  if (!delta_bounds_.try_reserve_delta(delta_before, delta_after, add_before,
                                       add_after, sub_before, sub_after)) {
    return ctx.conflict(DbConflictCode::kDbConflictBoundsExceeded);
  }

  return 0;
}

void
BalanceLock::add_committed_credit(int128_t delta)
{
  CHECK_GT(delta, 0);
  auto ok = delta_bounds_.try_reserve_delta(0, delta, 0, delta, 0, 0);
  CHECK(ok);
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

int128_t
BalanceLock::get_committed_delta(LockContext const &ctx) const
{
  auto snapshot_value_opt = snapshot_balance_.snapshot_value();
  int128_t snapshot_value =
      snapshot_value_opt.has_value() ? *snapshot_value_opt : 0;

  // build_result is the value committed to the DB; subtracting the snapshot
  // value yields the net change. For a newly created balance this is the raw
  // delta less the storage cost charged in build_result, which is exactly what
  // the scheduler must observe.
  return build_result(ctx) - snapshot_value;
}

HogDbStatus<void>
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
    HogAge age = {
        .address = ctx.key.to_contract_address(),
        // We set the hog balance for all groups in [HogAge] to 0 to avoid the
        // issue described in #1511 on GitHub. Specifically, if we were to set
        // (let's say) the number of hogs in [kHogAgeGroupToSchedule] to
        // [value] rather than 0, the following would happen:
        // 1. A new contract is created at some address X and Y hogs are
        //    transferred to this address.
        // 2. hog_db creates a new HogAge struct for address X and writes Y hogs
        //    to the [kHogAgeGroupToSchedule] group in the struct.
        // 3. hog_db sends a [HogsTransferred] struct to the mempool which
        //    indicates that Y hogs were transferred to address X.
        // 4. In the mempool scheduler, Y hogs are added to the
        //    [kHogAgeGroupReceived] group for address X, so then address X's
        //    [HogAge] struct has Y + Y = 2Y hogs in total, which is incorrect.
        .num_hogs = {to_xdr(uint128_t{0}), to_xdr(uint128_t{0}),
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
