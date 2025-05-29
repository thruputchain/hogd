#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "coord.hh"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/hogexec/interface/rpc_interface.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"

namespace hogd {

class ExecCoordServer {
public:
  ExecCoordServer(ExecCard self_card, int poll_threads,
                  std::string const &coord_addr)
    : self_card_(std::move(self_card)),
      main_worker_pool_(available_parallelism()), x23_pool_(poll_threads),
      cp_(x23_pool_)
  {
    init_from_coord(coord_addr);
  }

  void await_shutdown_request()
  {
    if (rpc_interface_) {
      rpc_interface_->await_shutdown_request();
    }
  }

  ~ExecCoordServer() { await_shutdown_request(); }

private:
  // Get the database information from the coordinator, and then
  // start the exec to coord RPC interface (which finishes the initialization).
  void init_from_coord(std::string const &coord_addr);

  // Connects to the database and performs initialization of the executor.
  // Then begins serving RPCs to the coordinator.
  void start_rpc_service(DatabaseIdentifier identifier,
                         uint16_t self_executor_id);

  // Connect to all relevant database components.
  std::unique_ptr<DatabaseProxy>
  connect_to_database(DatabaseIdentifier identifier);

  std::optional<Coordinator1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>
  connect_to_coord_allocate_service(std::string const &coord_address,
                                    std::string const &coord_allocate_port);

  IntroExecResult get_exec_result(std::string const &coord_address,
                                  std::string const &coord_allocate_port);

  ExecCard self_card_;
  WorkerPool main_worker_pool_;
  // We want to wait for CoroPoll to shutdown before shutting down WorkerPool,
  // as CoroPoll shutdown makes sure no pending RPCs are in flight (which might
  // have work enqueued on the WorkerPool).
  x23::ThreadPool x23_pool_;
  x23::CoroPoll cp_;
  std::unique_ptr<ExecutorRpcInterface> rpc_interface_;
};

} // namespace hogd
