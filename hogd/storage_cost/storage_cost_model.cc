#include "hogd/storage_cost/storage_cost_model.h"

#include "hogd/base/types.h"
#include "storage_cost.hh"
#include "types.hh"

namespace hogd {

int128_t
StorageCostModel::get_bytestring_cost(size_t bytestring_len) const
{
  return kBaseItemCost + (int128_t{bytestring_len} * kBytestringCostPerByte);
}

int128_t
StorageCostModel::get_retrocausal_queue_cost(int64_t active_queue_size) const
{
  if (active_queue_size == 0) {
    return 0;
  }
  return kBaseItemCost + (active_queue_size * kRCQCostPerEntry);
}

int128_t
StorageCostModel::get_replay_cache_cost(size_t replay_cache_size) const
{
  return kBaseItemCost + (replay_cache_size * kReplayCacheCostPerEntry);
}

StorageCostModelXdr
StorageCostModel::to_xdr() const
{
  return StorageCostModelXdr{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = static_cast<uint64_t>(kBaseItemCost),
      .bytestring_cost_per_byte = kBytestringCostPerByte,
      .base_contract_deployment_cost =
          static_cast<uint64_t>(kBaseContractDeploymentCost),
      .contract_deployment_cost_per_byte =
          static_cast<uint64_t>(kContractDeploymentCostPerByte),
      .auth_per_key_cost = static_cast<uint64_t>(kAuthPerKeyCost),
      .rcq_cost_per_entry = static_cast<uint64_t>(kRCQCostPerEntry),
      .replay_cache_cost_per_entry =
          static_cast<uint64_t>(kReplayCacheCostPerEntry),
      .balance_cost = static_cast<uint64_t>(kBalanceCost),
  };
}

} // namespace hogd
