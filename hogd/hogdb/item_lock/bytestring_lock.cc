#include "hogd/hogdb/item_lock/bytestring_lock.h"

#include <cassert>
#include <cstddef>
#include <span>

#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

HogDBStatus<int128_t>
BytestringLock::try_atomic_change_lock(const Bytes *before, const Bytes *after, const StorageCostModel *cost_model)
{
  assert(before == nullptr || before->value_view() == bytestring_);
  if (after != nullptr && after->value_view() != bytestring_) {
    return HogDBError{HogDBCode::kConflict};
  }
  return get_storage_cost_of_committed_value(cost_model);
}

} // namespace hogd