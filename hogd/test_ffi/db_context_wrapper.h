#pragma once

#include "hogd/hogdb/hogdb_core.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "rpc.hh"
#include "rpc_types.hh"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"
#include <cassert>
#include <cstdlib>
#include <memory>
#include <string>

namespace hogd {

class DBContext {
public:
  using db_client_t = DatabaseToCoordInterfaceV1::client<
      x23::RpcStream<x23::RpcLogger>::ClientRef>;

  const std::string db_to_exec_port;
  const std::string db_to_coord_port;
  const std::string exec_to_coord_port;

  DBContext();

  void advance_new_block()
  {
    if (current_block_id_ != 0) {
      if (db_client_.close_snapshot(current_block_id_)() != RPCError::kNone) {
        exit(-1);
      }
    }
    if (db_client_.open_database_snapshot(current_block_id_,
                                          current_block_id_ + 1)() !=
        RPCError::kNone) {
      exit(-1);
    }
    syscall_logfd = 1;
    current_block_id_++;
    snapshot_cache_ =
        std::make_unique<StorageSnapshotCache>(current_block_id_, db_proxy_);
  }

  bool is_block_open() const { return current_block_id_ != 0; }

  BlockID current_block_id() const { return current_block_id_; }

  TransactionID allocate_tx_id() { return next_tx_id_++; }

  StorageSnapshotCache &get_snapshot()
  {
    assert(is_block_open());
    assert(snapshot_cache_);
    return *snapshot_cache_;
  }

  PerTransactionDatabaseProxy get_per_tx_db_proxy()
  {
    return PerTransactionDatabaseProxy(current_block_id_, allocate_tx_id(),
                                       db_proxy_);
  }

private:
  HogdbCore db_;
  x23::ThreadPool pool_;
  x23::CoroPoll cp_;
  // assumes genesis is 0
  BlockID current_block_id_ = 0;
  TransactionID next_tx_id_ = 0;

  db_client_t db_client_;

  DatabaseProxy db_proxy_;

  std::unique_ptr<StorageSnapshotCache> snapshot_cache_ = nullptr;
};

} // namespace hogd
