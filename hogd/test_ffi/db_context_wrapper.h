#pragma once

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <memory>

#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "rpc_types.hh"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace hogd {

// Database-related context for the test environment.
// Provides appropriate methods for manipulating the evolution
// of the database during a test.
class DBContext {
public:
  DBContext(uint32_t db_read_port);

  void advance_new_block();

  StorageSnapshotCache &get_snapshot()
  {
    assert(snapshot_cache_);
    return *snapshot_cache_;
  }

private:
  x23::ThreadPool pool_;
  x23::CoroPoll cp_;
  DatabaseProxy db_proxy_;
  BlockID current_block_id_ = 0;
  std::unique_ptr<StorageSnapshotCache> snapshot_cache_ = nullptr;
};

} // namespace hogd
