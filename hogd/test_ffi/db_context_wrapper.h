#pragma once

#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include <cassert>
#include <cstdlib>
#include <memory>

namespace hogd {

class DBContext {
public:
  DBContext();

  void advance_new_block();

  StorageSnapshotCache &get_snapshot()
  {
    assert(snapshot_cache_);
    return *snapshot_cache_;
  }

private:
  std::unique_ptr<StorageSnapshotCache> snapshot_cache_ = nullptr;
};

} // namespace hogd
