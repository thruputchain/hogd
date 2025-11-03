#include "hogd/hogdb/item_lock/exclusive_lock.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include <cassert>

namespace hogd {

HogDBStatus<int128_t>
ExclusiveLock::try_atomic_change_lock(const ItemChange * /*unused*/,
                                      const ItemChange *after,
                                      const StorageCostModel *cost_model)
{
  assert(after != nullptr);
  assert(cost_model != nullptr);

  // TODO(geoff): This should be contained inside ItemChange;
  // but I do not want to mess storage accounting inside ItemChange yet

  if (after->is_bytes()) {
    return cost_model->get_bytestring_cost(after->to_bytes().size());
  }
  if (after->is_nnint128()) {
    return cost_model->kBaseItemCost;
  }
  if (after->is_deletion()) {
    return 0;
  }
  panic("unimplemented");
}

} // namespace hogd
