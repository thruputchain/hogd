#include "hogd/mempool/test/infra.h"

#include <memory>
#include <utility>

#include "hogd/hogdb/rpc_from_exec.h"
#include "hogd/hogdb/rpc_from_sched.h"
#include "hogd/mempool/registration.h"
#include "sched_db.hh"

namespace hogd {

MempoolTestInfra::MempoolTestInfra()
{
  // Multiple tests run within the same process, so we need to clear the
  // registration at the beginning of each test.
  Registration::registration().clear();

  // Start the mempool RPC servers. All of the subsystems register with the
  // mempool, so the mempool RPC servers need to start first.
  sched_db_server_.start_server(/*port=*/0);
  sched_event_server_.start_server(/*port=*/0);
  sched_exec_server_.start_server(/*port=*/0);
  mempool_coord_server_.start_server(/*port=*/0);

  auto sched_db_client = std::make_unique<sched_db_client_t>(
      create_client<SchedDb1>(kAddress, sched_db_server_.port()));
  // Initialize backend and blockchain state for tests.
  init_db_backend(std::move(sched_db_client));
  init_contracts();

  // Start the database RPC servers.
  // [db_executor_server_] handles RPCs from the executor.
  db_executor_server_ = std::make_unique<HogDBExecutorServer>(*ctx_);
  db_executor_server_->start_server(/*port=*/0);
  // [db_scheduler_server_] handles RPCs from the mempool.
  db_scheduler_server_ = std::make_unique<HogDBSchedServer>(*ctx_);
  db_scheduler_server_->start_server(/*port=*/0);

  // Start the event RPC servers.
  event_sched_server_.start_server(/*port=*/0);
  event_exec_server_.start_server(/*port=*/0);

  // We start the executor server in [register_executor_with_mempool()].

  // Start the fake coordinator. We need a coordinator to register itself with
  // the mempool. The real coordinator implementation is in the chuckd repo
  // and is written in go. So to ease test implementation, we use a fake
  // coordinator.
  coordinator_sched_server_.start_server(/*port=*/0);

  // Now have all of the subsystems register themselves with the mempool.
  register_db_with_mempool();
  register_event_with_mempool();
  // The executor must register itself *after* the database and event servers
  // have registered!
  register_executor_with_mempool();
  register_coordinator_with_mempool();
}

} // namespace hogd
