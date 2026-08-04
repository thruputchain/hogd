#pragma once

#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/item_lock/concepts.h"
#include "hogd/hog_db/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

class NewDeploymentLock {
public:
  using change_type = DeployedContractData;

  NewDeploymentLock(DeployedContractHash deployed_contract_hash)
    : deployed_contract_hash_(std::move(deployed_contract_hash))
  {}

  HogDbStatus<int128_t>
  try_atomic_change_lock(const DeployedContractData *before,
                         const DeployedContractData *after,
                         const LockContext &ctx);
  int128_t commit_change(bool did_finalize, int128_t snapshot_cost,
                         DeployedContractData & /*unused*/,
                         const LockContext &ctx) const;
  HogDbStatus<void> stage_committed_changes(const LockContext &ctx,
                                            ChangeSet &changes) const;

private:
  const DeployedContractHash deployed_contract_hash_;
};

static_assert(item_lock<NewDeploymentLock>);

} // namespace hogd
