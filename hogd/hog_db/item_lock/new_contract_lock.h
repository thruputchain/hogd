#pragma once

#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/item_lock/concepts.h"
#include "hogd/hog_db/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

class NewContractLock {
public:
  using change_type = NewContractData;

  NewContractLock(Contract contract) : contract_(std::move(contract)) {}

  HogDbStatus<int128_t> try_atomic_change_lock(const NewContractData *before,
                                               const NewContractData *after,
                                               const LockContext &ctx);
  static int128_t commit_change(bool /*unused*/, int128_t /*unused*/,
                                NewContractData & /*unused*/,
                                const LockContext & /*ctx*/)
  {
    return 0;
  }
  HogDbStatus<void> stage_committed_changes(const LockContext &ctx,
                                            ChangeSet &changes) const;

private:
  const Contract contract_;
};

static_assert(item_lock<NewContractLock>);

} // namespace hogd
