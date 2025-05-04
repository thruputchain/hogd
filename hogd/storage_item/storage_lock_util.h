#pragma once

#include "hogd/base/types.h"
#include "storage_item.hh"
#include "storage_lock.hh"

namespace hogd {

// A storage lock that tracks the number of tokens that must be locked to
// acquire it
struct MeteredStorageLock {
  StorageLock lock_;
  // Number of tokens that must be locked to acquire this lock
  int128_t locked_tokens_;

  bool operator==(const MeteredStorageLock &) const = default;
};

// Converts a storage delta into its corresponding storage item type class
// The typeclass is the set of all data that two StorageDeltas must agree on,
// (in a bytewise comparison), in order to be possibly concurrent.
StorageItemTypeClass delta_to_typeclass(StorageDelta const &delta);

// Creates a kSetTypeclass lock for the given storage delta
StorageLock delta_to_lock(StorageDelta const &delta);

} // namespace hogd
