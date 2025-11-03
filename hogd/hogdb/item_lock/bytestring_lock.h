#pragma once

#include <cstddef>
#include <utility>

#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

class BytestringLock {
public:
  BytestringLock(OwningSpan<const std::byte> bytestring)
    : bytestring_(std::move(bytestring))
  {}

  // On success, returns a (deterministic) upper bound on the storage cost
  // required to commit the after value. Is a no-op on failure.
  // If after is nullptr, the storage cost output should be ignored.
  HogDBStatus<int128_t>
  try_atomic_change_lock(const Bytes *before, const Bytes *after, const StorageCostModel *cost_model);

  int128_t
  get_storage_cost_of_committed_value(const StorageCostModel *cost_model) const noexcept
  {
    return cost_model->get_bytestring_cost(bytestring_.size());
  }
  
private:
  const OwningSpan<const std::byte> bytestring_;
};

} // namespace hogd
