#pragma once

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <memory>

#include "hogd/base/types.h"
#include "hogd/db_interface/block_manager.h"
#include "hogd/db_interface/db_executor_server.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/test_util/test_event_aggregator.h"
#include "hogd/mockdb/in_memory_database.h"
#include "rpc_types.hh"

namespace hogd {

// Database-related context for the test environment.
// Provides appropriate methods for manipulating the evolution
// of the database during a test.
class DBContext {
public:
  DBContext(uint32_t db_read_port = 0);

  void advance_new_block(BlockExecutionMode mode =
                             BlockExecutionMode::kBlockExecutionModeProposal);

  StorageSnapshotCache &get_snapshot()
  {
    assert(snapshot_cache_);
    return *snapshot_cache_;
  }

  std::pair<PerTransactionDatabaseProxy, PerTransactionEventServerProxy>
  get_new_db_proxy()
  {
    TransactionID tx_id = current_tx_id_.fetch_add(1);
    return std::make_pair(PerTransactionDatabaseProxy(*db_proxy_,
                                                      current_block_id_, tx_id),
                          PerTransactionEventServerProxy(*event_server_proxy_,
                                                         current_block_id_,
                                                         tx_id));
  }

  // Allows direct modification of the committed database -- not for use
  // within a contract, just within setup.
  auto &get_base_db() { return db_; }
  auto &get_block_manager() { return block_manager_; }

  BlockID get_current_block_id() const { return current_block_id_; }

private:
  BlockID current_block_id_ = 0;
  InMemoryDatabase db_;
  BlockManager block_manager_;
  SrpcDbExecutorServer executor_server_;
  TestEventAggregator event_server_;
  TestEventAggregatorExecRpc event_server_exec_rpc_ = TestEventAggregatorExecRpc(event_server_);
  
  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<EventServerProxy> event_server_proxy_;
  std::unique_ptr<StorageSnapshotCache> snapshot_cache_ = nullptr;
  std::atomic<TransactionID> current_tx_id_ = 0;
};

} // namespace hogd
