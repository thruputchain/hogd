#pragma once

#include <cassert>
#include <cstdlib>
#include <memory>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/test_util/block_snapshot_server.h"
#include "rpc_types.hh"
#include "types.hh"
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

  // Allows direct modification of the committed database -- not for use
  // within a contract, just within setup.
  auto &get_snapshot_server() { return snapshot_server_; }

private:
  x23::ThreadPool pool_;
  x23::CoroPoll cp_;
  BlockID current_block_id_ = 0;
  BlockSnapshotServer snapshot_server_;
  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<StorageSnapshotCache> snapshot_cache_ = nullptr;
};

} // namespace hogd
