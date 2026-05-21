#pragma once

#include "hogd/base/types.h"

#include "contract.hh"
#include "storage_cost.hh"
#include "types.hh"

namespace hogd {

struct StorageCostModel {
  // TODO(geoff): remove: this is for compat with the mockdb implementation.
  constexpr StorageCostModel(uint64_t base_item_cost,
                             uint32_t bytestring_cost_per_byte,
                             uint64_t rcq_cost_per_entry,
                             uint64_t replay_cache_cost_per_entry,
                             uint64_t balance_cost)
    : kBaseItemCost(base_item_cost),
      kBytestringCostPerByte(bytestring_cost_per_byte),
      kBaseContractDeploymentCost(kBaseItemCost),
      kContractDeploymentCostPerByte(1), kAuthPerKeyCost(1),
      kRCQCostPerEntry(rcq_cost_per_entry),
      kReplayCacheCostPerEntry(replay_cache_cost_per_entry),
      kBalanceCost(balance_cost)
  {}

  // Wrapper class that implements the storage cost model, given a particular
  // config.
  constexpr StorageCostModel(StorageCostModelXdr const &model)
    : kBaseItemCost(model.base_data_item_cost),
      kBytestringCostPerByte(model.bytestring_cost_per_byte),
      kBaseContractDeploymentCost(model.base_contract_deployment_cost),
      kContractDeploymentCostPerByte(model.contract_deployment_cost_per_byte),
      kAuthPerKeyCost(model.auth_per_key_cost),
      kRCQCostPerEntry(model.rcq_cost_per_entry),
      kReplayCacheCostPerEntry(model.replay_cache_cost_per_entry),
      kBalanceCost(model.balance_cost)
  {}

  StorageCostModel(StorageCostModel const &) = default;
  StorageCostModel(StorageCostModel &&) noexcept = default;

  int128_t get_bytestring_cost(size_t bytestring_len) const;

  constexpr int128_t get_nnint_cost() const { return kBaseItemCost; }

  int128_t get_retrocausal_queue_cost(int64_t active_queue_size) const;
  int128_t get_replay_cache_cost(size_t replay_cache_size) const;

  int128_t get_deployment_cost(DeployedContractHash const &hash) const
  {
    if (hash.contract_type != ContractType::kWasmi) {
      return 0;
    }
    return kBaseContractDeploymentCost +
           (int128_t{hash.contract_size} * kContractDeploymentCostPerByte);
  }

  constexpr int128_t get_auth_data_cost(size_t num_keys) const
  {
    return kBaseItemCost + (int128_t{num_keys} * kAuthPerKeyCost);
  }

  constexpr int128_t get_balance_cost() const { return kBalanceCost; }

  const uint64_t kBaseItemCost;
  const uint32_t kBytestringCostPerByte;
  const uint64_t kBaseContractDeploymentCost;
  const uint64_t kContractDeploymentCostPerByte;
  const uint64_t kAuthPerKeyCost;

  const uint64_t kRCQCostPerEntry;

  const uint64_t kReplayCacheCostPerEntry;

  const uint64_t kBalanceCost;

  StorageCostModelXdr to_xdr() const;

  static constexpr StorageCostModel default_cost_model()
  {
    return StorageCostModel(StorageCostModelXdr{
        .ext = ExtensionPoint(0),
        .base_data_item_cost = 12000,
        .bytestring_cost_per_byte = 10,
        .base_contract_deployment_cost = 12000,
        .contract_deployment_cost_per_byte = 10,
        .auth_per_key_cost = 1111,
        .rcq_cost_per_entry = 8,
        .replay_cache_cost_per_entry = 7,
        .balance_cost = 1234,
    });
  }
};

} // namespace hogd
