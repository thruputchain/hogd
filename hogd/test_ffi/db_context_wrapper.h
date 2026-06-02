#pragma once

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <optional>

#include "auth_rpc.hh"
#include "db_sched.hh"
#include "event.hh"
#include "hogd/auth/auth_server.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/test_util/hogdb_wrapper.h"
#include "hogd/hogexec/runtime/test_util/test_event_aggregator.h"
#include "rpc_types.hh"
#include "storage_cost.hh"
#include "transaction.hh"
#include "x23/srpc.h"
#include "x23/vec.h"

namespace hogd {

// Database-related context for the test environment.
// Provides appropriate methods for manipulating the evolution
// of the database during a test.
class DBContext {
public:
  DBContext(StorageCostModelXdr const &cost_model);

  void advance_new_block(BlockExecutionMode mode =
                             BlockExecutionMode::kBlockExecutionModeProposal);

  struct EphemeralTxIdentifier {
    BlockID block_id;
    TransactionID tx_id;
  };

  StorageSnapshotCache &get_snapshot()
  {
    assert(snapshot_cache_);
    return *snapshot_cache_;
  }

  EphemeralTxIdentifier allocate_identifier()
  {
    return EphemeralTxIdentifier{.block_id = current_block_id_,
                                 .tx_id = allocate_txid()};
  }

  PerTransactionDatabaseProxy get_new_db_proxy(EphemeralTxIdentifier identifier)
  {
    return PerTransactionDatabaseProxy(*db_proxy_, identifier.block_id,
                                       identifier.tx_id);
  }

  PerTransactionEventServerProxy
  get_new_event_server_proxy(EphemeralTxIdentifier identifier)
  {
    return PerTransactionEventServerProxy(*event_server_proxy_,
                                          identifier.block_id,
                                          identifier.tx_id);
  }

  BlockID get_current_block_id() const { return current_block_id_; }

  // Test FFI handler lifetime tracking. A SyscallHandlerContext borrows this
  // DBContext, along with the snapshot cache and DB/event proxies derived from
  // it, by reference. Freeing the DB or advancing it to a new block while a
  // handler is still open would dangle those references, so the test FFI layer
  // rejects those operations while open_handler_count_ > 0 (issues #2433,
  // #2436).
  void register_open_handler() { ++open_handler_count_; }
  void unregister_open_handler()
  {
    assert(open_handler_count_ > 0);
    --open_handler_count_;
  }
  bool has_open_handlers() const { return open_handler_count_ > 0; }

  x23::Async<std::optional<TxEventLog>>
  get_tx_event_log(EphemeralTxIdentifier identifier)
  {
    return event_server_.get_events(identifier.block_id, identifier.tx_id);
  }

  auto &get_base_db() { return hogdb_wrapper_; }

  std::optional<WireStorageItem>
  get_snapshot_item(AddressAndKey const &addrkey);

  AuthErrorCode run_auth_checks(SignedTransaction const &tx)
  {
    x23::span<const SignedTransaction> txs(&tx, 1);
    auto dbtip = get_base_db().get_db().read_dbtip();
    CHECK(dbtip.has_value());
    auto res = auth_server_->verify_transactions(dbtip->height, txs)();
    CHECK_EQ(res.size(), 1U);
    return res[0];
  }

private:
  TransactionID allocate_txid() { return current_tx_id_.fetch_add(1); }

  using db_sched_client_t =
      DbSched1::client<x23::SRpcStream<x23::RpcLogger>::ClientRef>;

  BlockID current_block_id_ = 0;

  HogdbWrapper hogdb_wrapper_;

  std::unique_ptr<AuthServer> auth_server_;
  RpcServer<AuthDBRpc> auth_db_server_;

  db_sched_client_t db_sched_client_;

  TestEventAggregator event_server_;
  TestEventAggregatorExecServer event_exec_server_ =
      TestEventAggregatorExecServer(event_server_);

  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<EventServerProxy> event_server_proxy_;
  std::unique_ptr<StorageSnapshotCache> snapshot_cache_ = nullptr;
  std::atomic<TransactionID> current_tx_id_ = 0;
  // Number of SyscallHandlerContexts currently borrowing this DBContext.
  // Atomic to match current_tx_id_ and stay race-free if handlers are created
  // or closed from different threads on the same DBContext.
  std::atomic<int> open_handler_count_ = 0;
};

} // namespace hogd
