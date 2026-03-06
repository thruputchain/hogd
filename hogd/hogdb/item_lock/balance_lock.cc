#include "hogd/hogdb/item_lock/balance_lock.h"

#include <cassert>
#include <cstddef>
#include <span>

#include "hogd/base/bits.h"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/db_hash.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"

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
BalanceLock::write_committed_change(LockContext const &ctx) const
{

  assert(ctx.key.is_address_balance());

  auto value = build_result(ctx);

  assert(ctx.dao);
  return ctx.dao->write_address_balance(ctx.key.get_address(), value);
}

HogDBStatus<void>
BalanceLock::apply_db_hash_delta(const LockContext &ctx, LtHash16 &hash) const
{
  assert(ctx.key.is_address_balance());
  assert(ctx.dao);

  auto key = make_address_key(ctx.key.get_address(), Prefix::kBalance);
  int128_t after = build_result(ctx);
  int128_t encoded_after = cpu_to_le(after);
  replace_known_item(hash, *ctx.dao, std::as_bytes(std::span(key)), kNNInt128,
                     byte_view(encoded_after));
  return {};
}

} // namespace hogd
