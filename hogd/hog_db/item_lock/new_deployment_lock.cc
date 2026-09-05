#include "hogd/hog_db/item_lock/new_deployment_lock.h"

#include <cassert>

#include "db_conflict.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_db/change_set.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/item_lock/concepts.h"
#include "hogd/hog_db/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "x23/serialize.h"

namespace hogd {

HogDbStatus<int128_t>
NewDeploymentLock::try_atomic_change_lock(const DeployedContractData *before,
                                          const DeployedContractData *after,
                                          const LockContext &ctx)
{
  assert(before == nullptr ||
         before->deployed_contract_hash() == deployed_contract_hash_);

  if (after != nullptr &&
      after->deployed_contract_hash() != deployed_contract_hash_) {
    return ctx.conflict(DbConflictCode::kDbConflictValueMismatch);
  }

  return ctx.cost_model->get_deployment_cost(deployed_contract_hash_);
}

int128_t
NewDeploymentLock::commit_change(bool did_finalize, int128_t snapshot_cost,
                                 DeployedContractData & /*unused*/,
                                 const LockContext &ctx) const
{
  if (did_finalize) {
    return ctx.cost_model->get_deployment_cost(deployed_contract_hash_) -
           snapshot_cost;
  }
  return 0;
}

HogDbStatus<void>
NewDeploymentLock::stage_committed_changes(const LockContext &ctx,
                                           ChangeSet &changes) const
{
  CHECK(ctx.key.is_address_metadata());

  // Write deployed contract metadata (part of blockchain state).
  auto key = make_address_key(ctx.key.get_address(), Prefix::kAddressMetadata);
  auto serialized = x23::serialize(deployed_contract_hash_);
  changes.put(key, 0, serialized);
  return {};
}

} // namespace hogd
