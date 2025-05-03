#pragma once

#include "hogd/base/types.h"
#include "storage_item.hh"
#include "storage_lock.hh"

namespace hogd {

struct MeteredStorageLock {
  StorageLock lock_;
  int128_t locked_tokens_;

  bool operator==(const MeteredStorageLock &) const = default;
};

StorageItemTypeClass
delta_to_typeclass(StorageDelta const &delta);

StorageLock delta_to_lock(StorageDelta const &delta);

} // namespace hogd
