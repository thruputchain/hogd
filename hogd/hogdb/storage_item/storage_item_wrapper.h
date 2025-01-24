#pragma once

#include <variant>

#include "hogd/base/types.h"
#include "storage_item.hh"

namespace hogd {

// Wrapper around StorageItem that implements more complex data types
// specifically the RetrocausalQueue.

class StorageItemWrapper {
public:
  StorageItemWrapper(StorageItem storage_item);

  auto &bytestring(this auto &self)
  {
    return std::get<BytestringWrapper>(self.storage_item_).bytestring;
  }

  auto &nonnegative_int(this auto &self)
  {
    return std::get<NonnegativeIntWrapper>(self.storage_item_).nnint;
  }

private:
  struct BytestringWrapper {
    Bytestring bytestring;
  };

  struct NonnegativeIntWrapper {
    int128_t nnint;
  };

  struct RetrocausalQueueWrapper {
    // todo(geoff): implement
  };

  std::variant<BytestringWrapper, NonnegativeIntWrapper,
               RetrocausalQueueWrapper>
      storage_item_;
};

} // namespace hogd
