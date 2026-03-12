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
#include "hogd/hogdb/prepared.h"
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

HogDBStatus<void>
NewDeploymentLock::prepare_committed_changes(const LockContext &ctx,
                                             Prepared &prepared) const
{
  assert(ctx.key.is_address_metadata());

  // Write deployed contract metadata (part of blockchain state).
  auto key = make_address_key(ctx.key.get_address(), Prefix::kAddressMetadata);
  auto serialized = x23::serialize(deployed_contract_hash_);
  prepared.put(std::as_bytes(std::span(key)), 0,
               std::as_bytes(std::span(serialized)));

  // Write empty HogAge record (not part of blockchain state — Prepared
  // will skip hash element construction because is_state_item returns false
  // for kHogAge).
  auto hog_age_key = make_address_key(ctx.key.get_address(), Prefix::kHogAge);
  HogAge age{
      .address = ctx.key.get_address().get_raw(),
      .num_hogs = {to_xdr(uint128_t{0}), to_xdr(uint128_t{0}),
                   to_xdr(uint128_t{0})},
      .average_ages = {BlockHeight{0}, BlockHeight{0}, BlockHeight{0}},
  };
  auto age_buf = x23::serialize(age);
  prepared.put(std::as_bytes(std::span(hog_age_key)), 0,
               std::as_bytes(std::span(age_buf)));

  return {};
}

} // namespace hogd
