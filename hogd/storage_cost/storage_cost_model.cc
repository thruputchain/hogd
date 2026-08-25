#include "hogd/storage_cost/storage_cost_model.h"

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
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
  return kBaseItemCost + (int128_t{active_queue_size} * kRCQCostPerEntry);
}

int128_t
StorageCostModel::get_replay_cache_cost(size_t replay_cache_size) const
{
  return kBaseItemCost +
         (int128_t{replay_cache_size} * kReplayCacheCostPerEntry);
}

StorageCostModelXdr
StorageCostModel::to_xdr() const
{
  return StorageCostModelXdr{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = kBaseItemCost,
      .bytestring_cost_per_byte = kBytestringCostPerByte,
      .base_contract_deployment_cost = kBaseContractDeploymentCost,
      .contract_deployment_cost_per_byte = kContractDeploymentCostPerByte,
      .auth_per_key_cost = kAuthPerKeyCost,
      .rcq_cost_per_entry = kRCQCostPerEntry,
      .replay_cache_cost_per_entry = kReplayCacheCostPerEntry,
      .balance_cost = kBalanceCost,
  };
}

ConfigHashPreimage
make_config_hash_preimage(StorageCostModel const &storage_cost_model)
{
  return ConfigHashPreimage{
      .storage_cost_model = storage_cost_model.to_xdr(),
  };
}

ConfigHash
compute_config_hash(ConfigHashPreimage const &preimage)
{
  return sha256(preimage);
}

ConfigHash
compute_config_hash(StorageCostModelXdr const &storage_cost_model)
{
  return compute_config_hash(
      ConfigHashPreimage{.storage_cost_model = storage_cost_model});
}

} // namespace hogd
