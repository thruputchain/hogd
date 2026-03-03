#include "hogd/hogdb/item_lock/new_deployment_lock.h"

#include <cassert>
#include <span>

#include "db_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {

HogDBStatus<int128_t>
NewDeploymentLock::try_atomic_change_lock(const DeployedContractData *before,
                                          const DeployedContractData *after,
                                          const LockContext &ctx)
{
  assert(before == nullptr ||
         before->deployed_contract_hash() == deployed_contract_hash_);

  if (after != nullptr &&
      after->deployed_contract_hash() != deployed_contract_hash_) {
    return HogDBError{HogDBCode::kConflict};
  }

  return ctx.cost_model->get_contract_size_cost(
      deployed_contract_hash_.contract_size);
}

HogDBStatus<void>
NewDeploymentLock::write_committed_change(LockContext const &ctx) const
{
  assert(ctx.key.is_address_metadata());
  assert(ctx.dao);
  HogDBStatus<void> ret = ctx.dao->write_address_metadata(
      ctx.key.get_address(), deployed_contract_hash_);

  // Create an empty data record for this address for scheduler state;
  // the scheduler is responsible for populating this with nonzero data.
  auto r = ctx.dao->write_address_average_hog_age(HogAge{
      .address = ctx.key.get_address().get_raw(),
      .num_hogs = {to_xdr(uint128_t{0}), to_xdr(uint128_t{0}),
                   to_xdr(uint128_t{0})},
      .average_ages = {BlockHeight{0}, BlockHeight{0}, BlockHeight{0}},
  });

  CHECK(r.has_value());

  return ret;
}

int128_t
NewDeploymentLock::commit_change(bool did_finalize, int128_t snapshot_cost,
                                 const DeployedContractData & /*unused*/,
                                 const StorageCostModel *cost_model) const
{
  if (did_finalize) {
    return cost_model->get_contract_size_cost(
               deployed_contract_hash_.contract_size) -
           snapshot_cost;
  }
  return 0;
}

} // namespace hogd
