#pragma once

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"

#include "storage_cost.hh"

namespace hogd {

struct StorageCostModel {
  // TODO(geoff): remove: this is for compat with the mockdb implementation.
  constexpr StorageCostModel(int128_t base_item_cost,
                             int128_t bytestring_cost_per_byte,
                             int128_t rcq_cost_per_entry,
                             int128_t rcq_cost_per_block,
                             int128_t replay_cache_cost_per_entry)
    : kBaseItemCost(base_item_cost),
      kBytestringCostPerByte(bytestring_cost_per_byte),
      kBaseContractDeploymentCost(kBaseItemCost),
      kContractDeploymentCostPerByte(1), kRCQCostPerEntry(rcq_cost_per_entry),
      kRCQCostPerBlock(rcq_cost_per_block),
      kReplayCacheCostPerEntry(replay_cache_cost_per_entry)
  {}

  // Wrapper class that implements the storage cost model, given a particular
  // config.
  StorageCostModel(StorageCostModelXdr const &model)
    : kBaseItemCost(from_xdr(model.base_data_item_cost)),
      kBytestringCostPerByte(model.bytestring_cost_per_byte),
      kBaseContractDeploymentCost(
          from_xdr(model.base_contract_deployment_cost)),
      kContractDeploymentCostPerByte(
          from_xdr(model.contract_deployment_cost_per_byte))
  {}

  int128_t get_bytestring_cost(size_t bytestring_len) const;
  int128_t get_retrocausal_queue_cost(int64_t active_queue_size,
                                      uint64_t active_block_count) const;
  int128_t get_replay_cache_cost(size_t replay_cache_size) const;

  constexpr int128_t get_contract_size_cost(size_t contract_size) const
  {
    return contract_size * kContractDeploymentCostPerByte;
  }

  StorageCostModel(const StorageCostModel &) = delete;
  StorageCostModel &operator=(const StorageCostModel &) = delete;
  StorageCostModel(StorageCostModel &&) = delete;
  StorageCostModel &operator=(StorageCostModel &&) = delete;

  const int128_t kBaseItemCost;
  const int128_t kBytestringCostPerByte;
  const int128_t kBaseContractDeploymentCost;
  const int128_t kContractDeploymentCostPerByte;

  // TODO(geoff): make these part of the XDR, once we settle on
  // cost/design for the RCQs.
  const int128_t kRCQCostPerEntry = 0;
  const int128_t kRCQCostPerBlock = 0;
  const int128_t kReplayCacheCostPerEntry = 0;
};

} // namespace hogd
