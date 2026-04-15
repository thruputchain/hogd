#include "hogd/storage_item/storage_item_wrapper.h"

#include <cassert>
#include <utility>
#include <variant>

#include "address_auth.hh"
#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/replay_cache.h"
#include "hogd/storage_item/retrocausal_queue.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"

namespace hogd {

StorageItemWrapper::StorageItemWrapper(Bytestring bytestring,
                                       StorageCostModel const *cost_model)
  : storage_item_(BytestringWrapper{.bytestring_ = std::move(bytestring)}),
    cost_model_(cost_model)
{}
StorageItemWrapper::StorageItemWrapper(int128_t nnint_value,
                                       StorageCostModel const *cost_model)
  : storage_item_(NonnegativeIntWrapper{.nnint_ = nnint_value}),
    cost_model_(cost_model)
{}
StorageItemWrapper::StorageItemWrapper(RetrocausalQueue queue,
                                       StorageCostModel const *cost_model)
  : storage_item_(RetrocausalQueueWrapper{.queue_ = std::move(queue)}),
    cost_model_(cost_model)
{}
StorageItemWrapper::StorageItemWrapper(ReplayCache replay_cache,
                                       StorageCostModel const *cost_model)
  : storage_item_(ReplayCacheWrapper{.cache_ = std::move(replay_cache)}),
    cost_model_(cost_model)
{}
StorageItemWrapper::StorageItemWrapper(AddressAuthData auth_data,
                                       StorageCostModel const *cost_model)
  : storage_item_(AuthDataWrapper{.auth_data_ = std::move(auth_data)}),
    cost_model_(cost_model)
{}
StorageItemWrapper::StorageItemWrapper(
    DeployedContractHash deployed_contract_hash,
    StorageCostModel const *cost_model)
  : storage_item_(DeployedContractHashWrapper{
        .deployed_contract_hash_ = std::move(deployed_contract_hash)}),
    cost_model_(cost_model)
{}

StorageItemType
StorageItemWrapper::type() const
{
  return std::visit([](auto const &v) { return v.kItemType; }, storage_item_);
}

WireStorageItem
StorageItemWrapper::to_wire_item() const
{
  return std::visit([](auto const &v) { return v.to_wire_item(); },
                    storage_item_);
}

WireStorageItem
StorageItemWrapper::BytestringWrapper::to_wire_item() const
{
  WireStorageItem item(StorageItemType::kItemTypeBytestring);
  item.bytestring() = bytestring_;
  return item;
}

WireStorageItem
StorageItemWrapper::NonnegativeIntWrapper::to_wire_item() const
{
  WireStorageItem item(StorageItemType::kItemTypeNonnegativeInt);
  item.nnint() = to_xdr(nnint_);
  return item;
}

WireStorageItem
StorageItemWrapper::RetrocausalQueueWrapper::to_wire_item() const
{
  auto const &meta = queue_.get_active_stats();
  uint64_t active_size = meta.active_size;
  auto out = WireStorageItem(StorageItemType::kItemTypeRetrocausalQueue);
  out.metadata() = RetrocausalQueueMetadata{
      .consume_state =
          RetrocausalQueuePastBlockConsumeDelta{
              .max_fully_consumed_block_number =
                  queue_.get_lowest_block_number(),
              .partial_consumed_number = 0,
          },
      .max_live_block_num = queue_.get_current_block_number(),
      .live_entries = active_size,
  };
  return out;
}

WireStorageItem
StorageItemWrapper::ReplayCacheWrapper::to_wire_item()
{
  WireStorageItem item(StorageItemType::kItemTypeReplayCacheData);
  return item;
}

WireStorageItem
StorageItemWrapper::AuthDataWrapper::to_wire_item() const
{
  WireStorageItem item(StorageItemType::kItemTypeAuthData);
  item.auth_data() = auth_data_;
  return item;
}

WireStorageItem
StorageItemWrapper::DeployedContractHashWrapper::to_wire_item() const
{
  WireStorageItem item(StorageItemType::kItemTypeDeployedContractHash);
  item.deployed_contract_hash() = deployed_contract_hash_;
  return item;
}

int128_t
StorageItemWrapper::BytestringWrapper::storage_cost(
    StorageCostModel const *cost_model) const
{
  return cost_model->get_bytestring_cost(bytestring_.size());
}

int128_t
StorageItemWrapper::NonnegativeIntWrapper::storage_cost(
    StorageCostModel const *cost_model)
{
  return cost_model->kBaseItemCost;
}

int128_t
StorageItemWrapper::RetrocausalQueueWrapper::storage_cost(
    StorageCostModel const *cost_model) const
{
  auto rcq_stats = queue_.get_active_stats();
  return cost_model->get_retrocausal_queue_cost(rcq_stats.active_size);
}

int128_t
StorageItemWrapper::ReplayCacheWrapper::storage_cost(
    StorageCostModel const *cost_model) const
{
  return cost_model->get_replay_cache_cost(cache_.committed_size());
}

int128_t
StorageItemWrapper::AuthDataWrapper::storage_cost( // NOLINT
    StorageCostModel const *cost_model) const
{
  return cost_model
      ->kBaseItemCost; // TODO(geoff): vary size based on number of signers
                       // cost_model->get_auth_data_cost(auth_data_.size());
}

int128_t
StorageItemWrapper::DeployedContractHashWrapper::storage_cost(
    StorageCostModel const *cost_model) const
{
  return cost_model->get_deployment_cost(
      deployed_contract_hash_.contract_size);
}
int128_t
StorageItemWrapper::storage_cost(StorageCostModel const *cost_model) const
{
  if (cost_model == nullptr) {
    cost_model = cost_model_;
  }
  assert(cost_model);
  return std::visit([cost_model](
                        auto const &v) { return v.storage_cost(cost_model); },
                    storage_item_);
}

} // namespace hogd
