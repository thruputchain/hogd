#include "hogd/hog_db/item_lock/balance_lock.h"

#include <cassert>
#include <cstddef>
#include <span>

#include "db_conflict.hh"
#include "db_sched.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hog_db/change_set.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
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
        return ctx.conflict(DbConflictCode::InsufficientFunds);
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
  if (committed_overflow) {
    return ctx.conflict(DbConflictCode::Overflow);
  }
  if (committed < 0) {
    return ctx.conflict(DbConflictCode::InsufficientFunds);
  }

  // Reservation fails when this block's debits exhaust the balance as of the
  // block's start (add-capacity exhaustion is unreachable under the supply
  // bound), so it is a funds shortfall rather than a generic bounds failure.
  if (!delta_bounds_.try_reserve_delta(delta_before, delta_after, add_before,
                                       add_after, sub_before, sub_after)) {
    return ctx.conflict(DbConflictCode::InsufficientFunds);
  }

  return 0;
}

void
BalanceLock::add_committed_credit(int128_t delta)
{
  CHECK_GT(delta, 0);
  // Refund of a redundant storage reservation (a duplicate same-value write
  // whose row an earlier commit already materialized). Bump the committed delta
  // directly rather than reserving add-capacity, which a refund is not and
  // which can be exhausted near INT128_MAX. This cannot overflow the committed
  // balance because total native-token supply is bounded at INT128_MAX in the
  // native token precompile, and that bound includes storage-locked funds, so
  // any single balance plus the storage it reclaims stays <= total supply <=
  // INT128_MAX. The safety comes from that global bound, not from the refund
  // reversing a specific reserved debit (it need not). (#4095)
  delta_bounds_.apply_committed_refund(delta);
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

  // Create the NativeTokenAge struct when the balance entry is created for the
  // first time (not part of blockchain state — ChangeSet will skip hash element
  // construction because is_state_item returns false for kNativeTokenAge).
  if (!snapshot_balance_.snapshot_value().has_value()) {
    NativeTokenAge age = {
        .address = ctx.key.to_contract_address(),
        // We set the native-token balance for all groups in [NativeTokenAge] to
        // 0 to avoid the issue described in #1511 on GitHub. Specifically, if
        // we were to set (let's say) the number of native tokens in
        // [kNativeTokenAgeGroupToSchedule] to [value] rather than 0, the
        // following would happen:
        // 1. A new contract is created at some address X and Y native tokens
        // are
        //    transferred to this address.
        // 2. hog_db creates a new NativeTokenAge struct for address X and
        // writes Y native tokens
        //    to the [kNativeTokenAgeGroupToSchedule] group in the struct.
        // 3. hog_db sends a [NativeTokensTransferredChunk] struct to the
        // mempool which
        //    indicates that Y native tokens were transferred to address X.
        // 4. In the mempool scheduler, Y native tokens are added to the
        //    [kNativeTokenAgeGroupReceived] group for address X, so then
        //    address X's [NativeTokenAge] struct has Y + Y = 2Y native tokens
        //    in total, which is incorrect.
        .num_native_tokens = {to_xdr(uint128_t{0}), to_xdr(uint128_t{0}),
                              to_xdr(uint128_t{0})},
        .average_ages = {BlockHeight{0}, BlockHeight{0}, BlockHeight{0}},
    };
    auto native_token_age_key =
        make_address_key(ctx.key.get_address(), Prefix::kNativeTokenAge);
    // Persist only the trailing fields; the key already encodes the address, so
    // storing it in the value too is redundant (issue #3812). address is
    // NativeTokenAge's first XDR field, and
    // read_address_average_native_token_age reconstructs it from the key.
    auto age_buf = x23::serialize(age);
    changes.put(native_token_age_key, 0,
                std::as_bytes(std::span(age_buf)).subspan(kAddressSize));
  }

  auto encoded = int128_to_xdr_bytes(value);
  auto key = make_address_key(ctx.key.get_address(), Prefix::kBalance);
  changes.put(key, kNNInt128, encoded);
  return {};
}

} // namespace hogd
