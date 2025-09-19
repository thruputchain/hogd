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

// Server that handles RPC communication between the executor and coordinator.
// Constructor connects to the coordinator to receive information about the
// database, then connects to the database and begins serving RPCs to the
// coordinator.
class ExecCoordServer {
public:
  ExecCoordServer(ExecCard self_card, const std::string &coord_address,
                  uint16_t coord_port, int poll_threads)
    : self_card_(std::move(self_card)), coord_address_(coord_address),
      coord_port_(coord_port), main_worker_pool_(available_parallelism()),
      x23_pool_(poll_threads), cp_(x23_pool_)
  {}

  ~ExecCoordServer() { await_shutdown_request(); }

  uint16_t start_server(uint16_t port_number)
  {
    return rpc_interface_->start_server(port_number);
  }

  void shut_down()
  {
    if (rpc_interface_) {
      rpc_interface_->shut_down();
    }
  }

  void await_shutdown_request()
  {
    if (rpc_interface_) {
      rpc_interface_->await_shutdown_request();
    }
  }

  // Get the database information from the coordinator, and then
  // start the exec to coord RPC interface (which finishes the initialization).
  void init_from_coord(const LocalMockDB *db);

private:
  // Connects to the database and performs initialization of the executor.
  // Then begins serving RPCs to the coordinator.
  void start_rpc_service(DatabaseIdentifier identifier,
                         uint16_t self_executor_id);

  // Connect to all relevant database components.
  static std::unique_ptr<DatabaseProxy>
  connect_to_database(DatabaseIdentifier identifier);

  std::optional<Coordinator1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>
  connect_to_coord_allocate_service();

  IntroExecResult get_exec_result();

  ExecCard self_card_;
  const std::string coord_address_;
  const uint16_t coord_port_;
  WorkerPool main_worker_pool_;
  // We want to wait for CoroPoll to shutdown before shutting down WorkerPool,
  // as CoroPoll shutdown makes sure no pending RPCs are in flight (which might
  // have work enqueued on the WorkerPool).
  x23::ThreadPool x23_pool_;
  x23::CoroPoll cp_;
  std::unique_ptr<ExecutorRpcInterface> rpc_interface_;
};

} // namespace hogd
