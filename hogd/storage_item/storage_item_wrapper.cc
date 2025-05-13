#include "hogd/storage_item/storage_item_wrapper.h"

#include <utility>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/storage_item/retrocausal_queue.h"
#include "hogd/storage_item/storage_costs.h"
#include "storage_item.hh"

namespace hogd {

StorageItemWrapper::StorageItemWrapper(BytestringMarker, Bytestring bytestring)
  : storage_item_(BytestringWrapper{.bytestring_ = std::move(bytestring)})
{}
StorageItemWrapper::StorageItemWrapper(NonnegativeIntMarker,
                                       int128_t nnint_value)
  : storage_item_(NonnegativeIntWrapper{.nnint_ = nnint_value})
{}
StorageItemWrapper::StorageItemWrapper(RetrocausalQueueMarker,
                                       RetrocausalQueue queue)
  : storage_item_(RetrocausalQueueWrapper{.queue_ = std::move(queue)})
{}
StorageItemWrapper::StorageItemWrapper(AssetBalanceMarker,
                                       uint128_t asset_balance)
  : storage_item_(AssetBalanceWrapper{.asset_balance_ = asset_balance})
{}

StorageItemType
StorageItemWrapper::type() const
{

  return std::visit([](auto const &v) { return v.kItemType; }, storage_item_);
}

StorageItem
StorageItemWrapper::to_wire_item() const
{
  return std::visit([](auto const &v) { return v.to_wire_item(); },
                    storage_item_);
}

StorageItem
StorageItemWrapper::BytestringWrapper::to_wire_item() const
{
  StorageItem item(StorageItemType::kItemTypeBytestring);
  item.bytestring() = bytestring_;
  return item;
}

StorageItem
StorageItemWrapper::NonnegativeIntWrapper::to_wire_item() const
{
  StorageItem item(StorageItemType::kItemTypeNonnegativeInt);
  item.nnint() = to_xdr(nnint_);
  return item;
}

StorageItem
StorageItemWrapper::RetrocausalQueueWrapper::to_wire_item()
{
  return StorageItem(StorageItemType::kItemTypeRetrocausalQueue);
}

StorageItem
StorageItemWrapper::AssetBalanceWrapper::to_wire_item() const
{
  StorageItem item(StorageItemType::kItemTypeAssetBalance);
  item.asset_balance() = to_xdr(asset_balance_);
  return item;
}

int128_t
StorageItemWrapper::BytestringWrapper::storage_cost() const
{
  return kBaseItemCost + bytestring_cost(bytestring_.size());
}

int128_t
StorageItemWrapper::NonnegativeIntWrapper::storage_cost()
{
  return kBaseItemCost;
}

int128_t
StorageItemWrapper::RetrocausalQueueWrapper::storage_cost() const
{
  return kBaseItemCost + retrocausal_queue_cost(queue_.get_active_stats());
}

int128_t
StorageItemWrapper::AssetBalanceWrapper::storage_cost()
{
  return kBaseItemCost;
}

int128_t
StorageItemWrapper::storage_cost() const
{
  return std::visit([](auto const &v) { return v.storage_cost(); },
                    storage_item_);
}

} // namespace hogd
