#pragma once

#include <variant>

#include "hogd/base/types.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/retrocausal_queue.h"
#include "storage_item.hh"

namespace hogd {

// Wrapper around StorageItem that implements more complex data types
// (specifically the RetrocausalQueue).
//
// StorageItemWrapper is not threadsafe.
class StorageItemWrapper {
public:
  StorageItemWrapper(Bytestring bytestring, StorageCostModel const *cost_model);
  StorageItemWrapper(int128_t nnint_value, StorageCostModel const *cost_model);
  StorageItemWrapper(RetrocausalQueue queue,
                     StorageCostModel const *cost_model);

  auto &bytestring(this auto &self)
  {
    return std::get<BytestringWrapper>(self.storage_item_).bytestring_;
  }

  auto &nnint(this auto &self)
  {
    return std::get<NonnegativeIntWrapper>(self.storage_item_).nnint_;
  }

  auto &queue(this auto &self)
  {
    return std::get<RetrocausalQueueWrapper>(self.storage_item_).queue_;
  }

  StorageItemType type() const;
  StorageItem to_wire_item() const;

  int128_t storage_cost(StorageCostModel const *cost_model = nullptr) const;

private:
  // Wrapper for bytestring storage items
  struct BytestringWrapper {
    // The underlying bytestring value
    Bytestring bytestring_;
    constexpr static StorageItemType kItemType =
        StorageItemType::kItemTypeBytestring;
    // Converts to wire format for network transmission
    StorageItem to_wire_item() const;
    // Returns storage cost based on bytestring length
    int128_t storage_cost(StorageCostModel const *cost_model) const;
  };

  // Wrapper for non-negative integer storage items
  struct NonnegativeIntWrapper {
    // The underlying non-negative integer value
    int128_t nnint_;
    constexpr static StorageItemType kItemType =
        StorageItemType::kItemTypeNonnegativeInt;
    // Converts to wire format for network transmission
    StorageItem to_wire_item() const;
    // Returns fixed storage cost for non-negative integers
    static int128_t storage_cost(StorageCostModel const *cost_model);
  };

  // Wrapper for retrocausal queue storage items
  struct RetrocausalQueueWrapper {
    // The underlying retrocausal queue
    RetrocausalQueue queue_;
    constexpr static StorageItemType kItemType =
        StorageItemType::kItemTypeRetrocausalQueue;
    // Converts to wire format for network transmission
    // Note: The RCQ type is automatically fully serialized and transferred.
    // We merely send over the fact that an RCQ exists.
    // Other subsystems can then query individual blocks within the RCQ.
    static StorageItem to_wire_item();
    // Returns storage cost based on total queue size and block count
    int128_t storage_cost(StorageCostModel const *cost_model) const;
  };

  std::variant<BytestringWrapper, NonnegativeIntWrapper,
               RetrocausalQueueWrapper>
      storage_item_;
  const StorageCostModel *cost_model_;
};

} // namespace hogd
