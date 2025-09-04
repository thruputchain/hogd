#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "coord.hh"
#include "db_coord.hh"
#include "hogd/base/types.h"
#include "hogd/genesis/genesis_block.h"
#include "hogd/hogexec/bench/control_client.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"

namespace hogd {

class CommonBenchState {
public:
  CommonBenchState(std::string server_addr, uint16_t exec_port,
                   uint16_t coord_port, uint16_t ci_server_port)
    : server_addr_(std::move(server_addr)), exec_port_(exec_port),
      coord_port_(coord_port), ci_server_port_(ci_server_port)
  {
    start_db_server();
    connect_coord();
    connect_exec();
  }

  ~CommonBenchState() { stop_db_server(); }

  SystemCallHandler make_handler(SignedTransaction const &signed_tx,
                                 TransactionID txid = 0)
  {
    if (txid == 0) {
      txid = allocate_txid();
    }
    auto proxy = PerTransactionDatabaseProxy(*db_proxy_, block_number_, txid);
    return SystemCallHandler(signed_tx, *snapshot_cache_, proxy,
                             ProposalModeData{}, make_hogexec_context());
  }

  void advance_block();
  void run_genesis_block(GenesisBlockBuilder &genesis_builder);

private:
  void connect_coord()
  {
    coord_client_ = [&] -> std::unique_ptr<coord_client_t> {
      while (true) {
        auto fd = cp_.tcpconnect(server_addr_, std::to_string(coord_port_))();
        if (!fd) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
        }
        auto client_config = x23::rpc_stream_logged(cp_, *fd);
        return std::make_unique<coord_client_t>(
            client_config.template client<DbCoord1>());
      }
    }();
  }

  void connect_exec()
  {
    std::string server_addrport =
        server_addr_ + ":" + std::to_string(exec_port_);
    db_proxy_ = std::make_unique<DatabaseProxy>(ShardedDB(IntroExecResult{
        .exec_id = 0,
        .shards = {ShardCard{.address = server_addrport.c_str()}}}));
  }

  void start_db_server()
  {
    auto ci_client = connect_to_control_server(cp_, server_addr_,
                                               std::to_string(ci_server_port_));
    ci_client.start_server("bench_server", {"-e", std::to_string(exec_port_),
                                            "-c", std::to_string(coord_port_)});
  }

  void stop_db_server()
  {
    auto ci_client = connect_to_control_server(cp_, server_addr_,
                                               std::to_string(ci_server_port_));
    ci_client.stop_server();
  }

  std::string server_addr_;
  uint16_t exec_port_;
  uint16_t coord_port_;

  uint16_t ci_server_port_;

  TransactionID allocate_txid()
  {
    return next_tx_id_.fetch_add(1, std::memory_order_relaxed);
  }

  std::atomic<TransactionID> next_tx_id_ = 0;
  BlockID block_number_ = 0;

  x23::ThreadPool db_pool_ = x23::ThreadPool(1);
  x23::CoroPoll cp_ = x23::CoroPoll(db_pool_);

  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<StorageSnapshotCache> snapshot_cache_;

  using coord_client_t =
      DbCoord1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

  std::unique_ptr<coord_client_t> coord_client_;
};

inline void
CommonBenchState::advance_block()
{
  auto r = coord_client_->dbcoord_close_snapshot(block_number_)();
  if (r.result() != DbResult::DbResultOK) {
    throw std::runtime_error("Failed to close snapshot");
  }

  auto r2 = coord_client_->dbcoord_commit_snapshot(block_number_)();
  if (r2.result() != DbResult::DbResultOK) {
    throw std::runtime_error("Failed to commit snapshot");
  }

  auto r3 = coord_client_->dbcoord_create_snapshot(
      block_number_, block_number_ + 1,
      BlockExecutionMode::kBlockExecutionModeProposal)();
  if (r3 != DbResult::DbResultOK) {
    throw std::runtime_error("Failed to create snapshot");
  }

  block_number_++;

  snapshot_cache_ =
      std::make_unique<StorageSnapshotCache>(block_number_, *db_proxy_);
}

// leaves the benchmark in a state where it's ready to execute transactions
inline void
CommonBenchState::run_genesis_block(GenesisBlockBuilder &genesis_builder)
{
  assert(block_number_ == 0);
  auto genesis_block = genesis_builder.build();
  {
    auto r = coord_client_->dbcoord_load_genesis_block(genesis_block)();
    if (r != DbResult::DbResultOK) {
      throw std::runtime_error("Failed to write genesis block");
    }
  }

  {
    auto r = coord_client_->dbcoord_create_snapshot(
        0, 1, BlockExecutionMode::kBlockExecutionModeProposal)();
    if (r != DbResult::DbResultOK) {
      throw std::runtime_error("Failed to create snapshot");
    }
  }

  block_number_++;
  snapshot_cache_ =
      std::make_unique<StorageSnapshotCache>(block_number_, *db_proxy_);

  auto unfurl_txs = genesis_builder.make_unfurl_genesis_transactions();
  for (auto &tx : unfurl_txs) {
    auto handler = make_handler(tx);
    if (!handler.root_invoke().is_root_success()) {
      throw std::runtime_error("Failed to invoke transaction");
    }
    if (auto r = handler.try_commit_transaction();
        !r.has_value() || !r.value()) {
      throw std::runtime_error("failed to perform commit operation");
    }
  }
  advance_block();
}

} // namespace hogd
