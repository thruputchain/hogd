#include "hogd/hogdb/item_lock/nnint128_lock.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <limits>

#include "db_conflict.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hogdb/change_set.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"

namespace hogd {

namespace detail {

bool
NNInt128DeltaBounds::try_reserve_delta(int128_t delta_before,
                                       int128_t delta_after,
                                       int128_t add_before, int128_t add_after,
                                       int128_t sub_before, int128_t sub_after)
{
  // assert inputs well formed
  assert(add_before >= 0);
  assert(add_after >= 0);
  assert(sub_before <= 0);
  assert(sub_after <= 0);

  // these calculations can only overflow if sub_after == 0 and sub_before ==
  // int128_min.
  //
  // However, it should never be possible to acquire a sub_before ==
  // int128_min lock, as that would require subtracing int128_min from an int128
  // base value without going negative, which is impossible (base max is
  // int128_max, and int128_max - int128_min is -1)
  assert(sub_before != std::numeric_limits<int128_t>::min());
  assert(delta_before != std::numeric_limits<int128_t>::min());
  int128_t add_delta = add_after - add_before;
  int128_t sub_delta = sub_after - sub_before;
  auto [delta_delta, overflow] = overflowing_sub(delta_after, delta_before);
  assert(!overflow);

  // add/sub bounds should bracket the delta
  assert(add_before >= delta_before);
  assert(sub_before <= delta_before);
  assert(add_after >= delta_after);
  assert(sub_after <= delta_after);

  int128_t add_lock = add_delta > 0 ? add_delta : 0;
  int128_t sub_lock = sub_delta < 0 ? sub_delta : 0;
  int128_t add_release = add_delta > 0 ? 0 : add_delta;
  int128_t sub_release = sub_delta < 0 ? 0 : sub_delta;

  assert(add_lock + add_release == add_delta);
  assert(sub_lock + sub_release == sub_delta);

  if (!try_reserve_add_capacity(add_lock)) {
    return false;
  }
  if (!try_reserve_sub_capacity(sub_lock)) {
    // release the locked add capacity
    release_add_capacity(add_lock);
    return false;
  }

  // no more locks to acquire
  if (delta_delta != 0) {
    delta_.fetch_add(delta_delta, std::memory_order_relaxed);
  }

  release_add_capacity(-add_release);
  // negation is safe
  assert(sub_release != std::numeric_limits<int128_t>::min());
  release_sub_capacity(-sub_release);

  return true;
}

} // namespace detail

HogDBStatus<int128_t>
NNInt128Lock::try_atomic_change_lock(const NNInt128 *before,
                                     const NNInt128 *after,
                                     const LockContext &ctx)
{
  assert(before == nullptr || before->base() == base_);

  if (after != nullptr && after->base() != base_) {
    return ctx.conflict(DbConflictCode::kDbConflictValueMismatch);
  }

  int128_t add_before = before == nullptr ? 0 : before->plus();
  int128_t sub_before = before == nullptr ? 0 : before->minus();
  int128_t add_after = after == nullptr ? 0 : after->plus();
  int128_t sub_after = after == nullptr ? 0 : after->minus();

  // Once we reserve the adds and subtracts, it must be safe to add to the
  // delta.
  int128_t delta_before = before == nullptr ? 0 : before->delta();
  int128_t delta_after = after == nullptr ? 0 : after->delta();

  if (!delta_bounds_.try_reserve_delta(delta_before, delta_after, add_before,
                                       add_after, sub_before, sub_after)) {
    return ctx.conflict(DbConflictCode::kDbConflictBoundsExceeded);
  }

  return get_storage_cost_of_committed_value(ctx.cost_model);
}

HogDBStatus<void>
NNInt128Lock::stage_committed_changes(const LockContext &ctx,
                                      ChangeSet &changes) const
{
  assert(!ctx.key.is_address_balance());
  const int128_t delta = delta_bounds_.get_delta();
  CHECK(is_committed_value_in_bounds(base_, delta));
  auto value = wrapping_add(base_, delta);
  auto encoded = int128_to_xdr_bytes(value);
  auto key = make_contract_data_key(ctx.key);
  changes.put(key, kNNInt128, encoded);
  return {};
}

} // namespace hogd
