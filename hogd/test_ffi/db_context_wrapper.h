#pragma once

#include "contract.hh"
#include "hogd/hogdb/hogdb_core.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "rpc.hh"
#include "rpc_types.hh"
#include "types.hh"
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

  void advance_new_block();

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

  void setup_create_contract(Contract const &contract);
  void setup_create_deployment(ContractAddress const &addr,
                               SHA256Hash const &hash);

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
