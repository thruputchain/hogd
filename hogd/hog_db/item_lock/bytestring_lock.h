#pragma once

#include <cstddef>
#include <utility>

#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/item_lock/concepts.h"
#include "hogd/hog_db/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

class BytestringLock {
public:
  BytestringLock(OwningSpan<const std::byte> bytestring)
    : bytestring_(std::move(bytestring))
  {}

  using change_type = Bytes;

  // On success, returns a (deterministic) upper bound on the storage cost
  // required to commit the after value. Is a no-op on failure.
  // If after is nullptr, the storage cost output should be ignored.
  HogDbStatus<int128_t> try_atomic_change_lock(const Bytes *before,
                                               const Bytes *after,
                                               const LockContext &ctx);

  int128_t get_storage_cost_of_committed_value(
      const StorageCostModel *cost_model) const noexcept
  {
    return cost_model->get_bytestring_cost(bytestring_.size());
  }

  int128_t commit_change(bool did_finalize, int128_t snapshot_cost,
                         Bytes & /*unused*/, const LockContext &ctx) const
  {
    if (did_finalize) {
      return ctx.cost_model->get_bytestring_cost(bytestring_.size()) -
             snapshot_cost;
    }
    return 0;
  }

  HogDbStatus<void> stage_committed_changes(const LockContext &ctx,
                                            ChangeSet &changes) const;

private:
  const OwningSpan<const std::byte> bytestring_;
};

static_assert(item_lock<BytestringLock>);

} // namespace hogd
