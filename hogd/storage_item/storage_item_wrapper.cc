#include "hogd/storage_item/storage_item_wrapper.h"

#include <variant>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/storage_item/retrocausal_queue.h"
#include "hogd/storage_item/storage_costs.h"
#include "storage_item.hh"

namespace hogd {

StorageItemWrapper::StorageItemWrapper(Bytestring const &bytestring)
  : storage_item_(BytestringWrapper{.bytestring_ = bytestring})
{}
StorageItemWrapper::StorageItemWrapper(int128_t nnint_value)
  : storage_item_(NonnegativeIntWrapper{.nnint_ = nnint_value})
{}
StorageItemWrapper::StorageItemWrapper(RetrocausalQueue const &queue)
  : storage_item_(RetrocausalQueueWrapper{.queue_ = queue})
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
  StorageItem item(StorageItemType::kBytestring);
  item.bytestring() = bytestring_;
  return item;
}

StorageItem
StorageItemWrapper::NonnegativeIntWrapper::to_wire_item() const
{
  StorageItem item(StorageItemType::kNonnegativeInt);
  item.nnint() = to_xdr(nnint_);
  return item;
}

StorageItem
StorageItemWrapper::RetrocausalQueueWrapper::to_wire_item()
{
  return StorageItem(StorageItemType::kRetrocausalQueue);
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
StorageItemWrapper::storage_cost() const
{
  return std::visit([](auto const &v) { return v.storage_cost(); },
                    storage_item_);
}

} // namespace hogd
