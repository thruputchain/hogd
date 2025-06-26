#pragma once

#include "hogd/base/types.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "storage_lock.hh"

namespace hogd {

class StorageItemWrapper;

// A storage lock that tracks the number of tokens that must be locked to
// acquire it
struct MeteredStorageLock {
  StorageLock lock_;
  // Number of tokens that must be locked to acquire this lock
  int128_t locked_tokens_;

  bool operator==(const MeteredStorageLock &) const = default;
};

// Creates a kSetTypeclass lock for the given storage delta
StorageLock delta_to_lock(StorageDelta const &delta, BlockExecutionMode mode,
                          StorageItemWrapper const *committed_item);

} // namespace hogd
