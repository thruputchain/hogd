#pragma once

#include <utility>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

// Builders for the storage items tests seed directly into a database or
// snapshot server.

inline WireStorageItem
make_bytestring_item(Bytestring bytestring)
{
  WireStorageItem item(StorageItemType::kItemTypeBytestring);
  item.bytestring() = std::move(bytestring);
  return item;
}

inline WireStorageItem
make_nnint_item(int128_t value)
{
  WireStorageItem item(StorageItemType::kItemTypeNonnegativeInt);
  item.nnint() = to_xdr(value);
  return item;
}

} // namespace hogd
