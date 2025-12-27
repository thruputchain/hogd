#pragma once

#include <memory>
#include <utility>

#include "contract.hh"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_util/hogdb_wrapper.h"
#include "hogd/hogexec/runtime/test_util/test_event_aggregator.h"
#include "storage_cost.hh"
#include "transaction.hh"
#include "x23/sockio.h"

namespace hogd {

// Boilerplate for testing just the wasm runtime integrations.
// Required because of how the runtime context lazily loads wasm contracts --
// this setup is what suffices to create a SystemCallHandler that can do nothing
// except supply those contracts.
//
// The alternative would be to add an extra layer of indirection
// within the syscall handler.
class WasmTestFramework {
public:
  // This handler is used to load contracts to the runtime context.
  std::unique_ptr<SystemCallHandler> syscall_handler_;

  void make_syscall_handler(Contract const &contract)
  {
    SignedTransaction signed_tx = SignedTransaction{};
    signed_tx.tx.contracts_to_deploy.push_back(contract);
    syscall_handler_ = std::make_unique<SystemCallHandler>(
        std::move(signed_tx), snapshot_cache_,
        PerTransactionDatabaseProxy(db_proxy_, 0, 0),
        PerTransactionEventServerProxy(event_server_proxy_, 0, 0),
        ProposalModeCtor{}, *runtime_context_);
  }

private:
  // Boilerplate irrelevant for the test, just for creating the syscall handler.
  HogdbWrapper hogdb_wrapper_ = HogdbWrapper(StorageCostModelXdr{});
  TestEventAggregator event_server_;
  TestEventAggregatorExecServer event_server_exec_rpc_ =
      TestEventAggregatorExecServer(event_server_);

  DatabaseProxy db_proxy_ = DatabaseProxy(
      LocalMockDB{.read_port_number_ = hogdb_wrapper_.exec_port()});
  StorageSnapshotCache snapshot_cache_ = StorageSnapshotCache(0, db_proxy_);
  EventServerProxy event_server_proxy_ = EventServerProxy(
      x23::HostPort{"localhost", event_server_exec_rpc_.start_server(0)});

  std::unique_ptr<RuntimeContext> runtime_context_ =
      make_runtime_context(RuntimeEngine::kWasmi);
};

} // namespace hogd
