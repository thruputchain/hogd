#pragma once

#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

class NewDeploymentLock {
public:
  using change_type = DeployedContractData;

  NewDeploymentLock(DeployedContractHash deployed_contract_hash)
    : deployed_contract_hash_(std::move(deployed_contract_hash))
  {}

  HogDBStatus<int128_t>
  try_atomic_change_lock(const DeployedContractData *before,
                         const DeployedContractData *after,
                         const LockContext &ctx);
  int128_t commit_change(bool did_finalize, int128_t snapshot_cost,
                         const DeployedContractData & /*unused*/,
                         const StorageCostModel *cost_model) const;
  HogDBStatus<void> write_committed_change(LockContext const &ctx) const;
  HogDBStatus<void> apply_db_hash_delta(const LockContext &ctx,
                                        LtHash16 &hash) const;

private:
  const DeployedContractHash deployed_contract_hash_;
};

static_assert(item_lock<NewDeploymentLock>);

} // namespace hogd
