#include "hogd/storage_cost/storage_cost_model.h"

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "storage_cost.hh"
#include "types.hh"

namespace hogd {

int128_t
StorageCostModel::get_bytestring_cost(size_t bytestring_len) const
{
  return kBaseItemCost + (bytestring_len * kBytestringCostPerByte);
}

int128_t
StorageCostModel::get_retrocausal_queue_cost(int64_t active_queue_size,
                                             uint64_t active_block_count) const
{
  return (active_queue_size * kRCQCostPerEntry) +
         (active_block_count * kRCQCostPerBlock);
}

int128_t
StorageCostModel::get_replay_cache_cost(size_t replay_cache_size) const
{
  return replay_cache_size * kReplayCacheCostPerEntry;
}

StorageCostModelXdr
StorageCostModel::to_xdr() const
{
  return StorageCostModelXdr{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = ::hogd::to_xdr(kBaseItemCost),
      .bytestring_cost_per_byte = kBytestringCostPerByte,
      .base_contract_deployment_cost =
          ::hogd::to_xdr(kBaseContractDeploymentCost),
      .contract_deployment_cost_per_byte =
          ::hogd::to_xdr(kContractDeploymentCostPerByte),
  };
}

} // namespace hogd
