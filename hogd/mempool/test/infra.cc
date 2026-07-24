#include "hogd/mempool/test/infra.h"

#include <memory>
#include <string>
#include <utility>

#include "db_sched.hh"
#include "hogd/auth/auth_server.h"
#include "hogd/auth/dbauth_client.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogdb/rpc_from_auth.h"
#include "hogd/hogdb/rpc_from_exec.h"
#include "hogd/hogdb/rpc_from_sched.h"
#include "hogd/hogexec/shared.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/mempool/registration.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/shared.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "sched_db.hh"

namespace hogd {

MempoolTestInfra::MempoolTestInfra(bool skip_genesis_block,
                                   size_t mempool_tx_size_limit)
  : mempool_coord_server_(
        RpcServer<MempoolCoordinatorRpc>(sched_db_server_->impl(),
                                         mempool_auth_server_->impl(),
                                         kNetworkHash, mempool_tx_size_limit))
{
  // Multiple tests run within the same process, so we need to clear the
  // registration and metrics at the beginning of each test.
  Registration::registration().clear();
  Metrics::instance().clear();
  init_hogexec_metrics();

  mempool_auth_server_->max_message_size_ = LoadgenRpc::kRpcMaxMessageSize;

  // Start the mempool RPC servers. All of the subsystems register with the
  // mempool, so the mempool RPC servers need to start first.
  sched_db_server_.start_server(/*port=*/0);
  sched_event_server_.start_server(/*port=*/0);
  sched_exec_server_.start_server(/*port=*/0);
  mempool_coord_server_.start_server(/*port=*/0);
  mempool_auth_server_.start_server(/*port=*/0);

  auto sched_db_client = std::make_unique<sched_db_client_t>(
      create_client<SchedDb1>(kAddress, sched_db_server_.port())().value());
  // Initialize backend and blockchain state for tests.
  init_db_backend(std::move(sched_db_client));
  init_contracts();

  // Start the database RPC servers.
  // [db_executor_server_] handles RPCs from the executor.
  db_executor_server_ = std::make_unique<HogDBExecutorServer>(*ctx_);
  db_executor_server_->start_server(/*port=*/0);
  // [db_scheduler_server_] handles RPCs from the mempool.
  db_scheduler_server_ = std::make_unique<HogDBSchedServer>(*ctx_);
  (*db_scheduler_server_)->max_message_size_ = LoadgenRpc::kRpcMaxMessageSize;
  db_scheduler_server_->start_server(/*port=*/0);
  db_auth_server_ = std::make_unique<HogDBAuthServer>(*ctx_);
  db_auth_server_->start_server(/*port=*/0);

  // Start the auth servers.
  std::string dbauth_address = make_address(kAddress, db_auth_server_->port());
  dbauth_client_t dbauth_client = make_dbauth_client(dbauth_address)().value();
  auth_server_ = std::make_unique<AuthServer>(std::move(dbauth_client));

  auth_db_server_->max_message_size_ = LoadgenRpc::kRpcMaxMessageSize;
  auth_mempool_server_->max_message_size_ = LoadgenRpc::kRpcMaxMessageSize;
  auth_db_server_.start_server(/*port=*/0);
  auth_mempool_server_.start_server(/*port=*/0);
  auth_db_server_->impl()->set_auth_server(*auth_server_);
  auth_mempool_server_->impl()->set_auth_server(*auth_server_);

  // Start the event RPC servers.
  event_sched_server_.start_server(/*port=*/0);
  event_exec_server_.start_server(/*port=*/0);

  // We start the executor server in [register_executor_with_mempool()].

  // Start the fake coordinator. We need a coordinator to register itself with
  // the mempool. The real coordinator implementation is in the chuckd repo
  // and is written in go. So to ease test implementation, we use a fake
  // coordinator.
  coordinator_sched_server_->max_message_size_ = LoadgenRpc::kRpcMaxMessageSize;
  coordinator_sched_server_.start_server(/*port=*/0);

  // Now have all of the subsystems register themselves with the mempool.
  register_db_with_mempool();
  register_event_with_mempool();
  register_auth_with_mempool();
  // The executor must register itself *after* the database and event servers
  // have registered!
  register_executor_with_mempool();
  register_coordinator_with_mempool()();

  if (!skip_genesis_block) {
    GenesisConfigurationBuilder builder;
    CHECK_EQ(
        mempool_coord_server_->impl()->schedcoord_load_genesis_configuration(
            builder.build_or_panic(StorageCostModel::default_cost_model()))(),
        DbSchedResult::DbSchedResultOK);
  }
}

} // namespace hogd
