#include "hogd/hogdb/item_lock/bytestring_lock.h"

#include <cassert>
#include <cstddef>
#include <span>

#include "db_conflict.hh"
#include "hogd/base/types.h"
#include "hogd/hogdb/change_set.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"

namespace hogd {

HogDBStatus<int128_t>
BytestringLock::try_atomic_change_lock(const Bytes *before, const Bytes *after,
                                       const LockContext &ctx)
{
  assert(before == nullptr || before->value_view() == bytestring_);
  if (after != nullptr && after->value_view() != bytestring_) {
    return ctx.conflict(DbConflictCode::kDbConflictValueMismatch);
  }
  return get_storage_cost_of_committed_value(ctx.cost_model);
}

HogDBStatus<void>
BytestringLock::stage_committed_changes(const LockContext &ctx,
                                        ChangeSet &changes) const
{
  auto key = make_contract_data_key(ctx.key);
  changes.put(key, kBytes, bytestring_);
  return {};
}

} // namespace hogd
