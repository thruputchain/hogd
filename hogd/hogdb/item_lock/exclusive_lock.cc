#include "hogd/hogdb/item_lock/exclusive_lock.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include <cassert>

namespace hogd {

// NOLINTBEGIN(readability-convert-member-functions-to-static)
HogDBStatus<int128_t>
ExclusiveLock::try_atomic_change_lock(const ItemChange * /*unused*/,
                                      const ItemChange *after,
                                      const LockContext &ctx)
{
  assert(after != nullptr);
  assert(ctx.cost_model != nullptr);

  // TODO(geoff): This should be contained inside ItemChange;
  // but I do not want to mess storage accounting inside ItemChange yet

  if (after->is_bytes()) {
    return ctx.cost_model->get_bytestring_cost(after->to_bytes().size());
  }
  if (after->is_nnint128()) {
    return ctx.cost_model->kBaseItemCost;
  }
  if (after->is_deletion()) {
    return 0;
  }
  panic("unimplemented");
}
// NOLINTEND(readability-convert-member-functions-to-static)

} // namespace hogd
