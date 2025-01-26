#pragma once

#include <variant>

#include "hogd/base/types.h"
#include "hogd/hogdb/storage_item/retrocausal_queue.h"
#include "storage_item.hh"

namespace hogd {

// Wrapper around StorageItem that implements more complex data types
// specifically the RetrocausalQueue.

class StorageItemWrapper {
public:
  StorageItemWrapper(Bytestring const &bytestring);
  StorageItemWrapper(int128_t nnint_value);
  StorageItemWrapper(RetrocausalQueue const &queue);

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

private:
  struct BytestringWrapper {
    Bytestring bytestring_;
    constexpr static StorageItemType kItemType = StorageItemType::kBytestring;
    StorageItem to_wire_item() const;
  };

  struct NonnegativeIntWrapper {
    int128_t nnint_;
    constexpr static StorageItemType kItemType =
        StorageItemType::kNonnegativeInt;
    StorageItem to_wire_item() const;
  };

  struct RetrocausalQueueWrapper {
    RetrocausalQueue queue_;
    constexpr static StorageItemType kItemType =
        StorageItemType::kRetrocausalQueue;
    static StorageItem to_wire_item();
  };

  std::variant<BytestringWrapper, NonnegativeIntWrapper,
               RetrocausalQueueWrapper>
      storage_item_;
};

} // namespace hogd
