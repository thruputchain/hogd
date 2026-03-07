#pragma once

#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

class NewContractLock {
public:
  using change_type = NewContractData;

  NewContractLock(Contract contract) : contract_(std::move(contract)) {}

  HogDBStatus<int128_t> try_atomic_change_lock(const NewContractData *before,
                                               const NewContractData *after,
                                               const LockContext &ctx);
  static int128_t commit_change(bool /*unused*/, int128_t /*unused*/,
                                const NewContractData & /*unused*/,
                                const StorageCostModel * /*unused*/)
  {
    return 0;
  }
  HogDBStatus<void> write_committed_change(LockContext const &ctx) const;
  HogDBStatus<void> apply_db_hash_delta(const LockContext &ctx,
                                        LtHash16 &hash) const;

private:
  const Contract contract_;
};

static_assert(item_lock<NewContractLock>);

} // namespace hogd
