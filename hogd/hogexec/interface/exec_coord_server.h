#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/interface/rpc_interface.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "sched_exec.hh"
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
  ExecCoordServer(std::string coordinator_host, uint16_t coordinator_port)
    : coordinator_host_(std::move(coordinator_host)),
      coordinator_port_(coordinator_port), cp_(x23_pool_),
      rpc_interface_(std::make_unique<ExecutorRpcInterface>(cp_))
  {}

  ~ExecCoordServer() { await_shutdown_request(); }

  uint16_t start_server(uint16_t port_number)
  {
    // When we register with the coordinator, we need to tell the coordinator
    // our port number. However, we do not know our port number until we
    // actually start our server (especially because [port_number] could be 0,
    // in which case the kernel will assign a port number to us).
    // Thus, we start the RPC server first, then below we register with the
    // coordinator, and finally we start handling incoming RPCs in
    // [start_rpc_service()].
    rpc_interface_->start_server(port_number);
    IntroExecResult exec_result = register_with_coordinator();
    start_rpc_service(ShardedDB(exec_result), exec_result.exec_id);

    return rpc_interface_->port();
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

private:
  // Generates the address of this executor, which includes both the hostname
  // and the port of the executor RPC server. We send this address to the
  // coordinator when we register this executor with it.
  std::string get_executor_address() const;

  // Connects to the database and performs initialization of the executor.
  // Then begins serving RPCs to the coordinator.
  void start_rpc_service(DatabaseIdentifier identifier,
                         uint16_t self_executor_id);

  // Create an RPC client that is connected to the coordinator.
  std::optional<SchedExec1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>
  connect_to_coordinator();

  // Register this executor with the coordinator.
  IntroExecResult register_with_coordinator();

  // We want to wait for CoroPoll to shutdown before shutting down WorkerPool,
  // as CoroPoll shutdown makes sure no pending RPCs are in flight (which might
  // have work enqueued on the WorkerPool).
  x23::ThreadPool x23_pool_;
  x23::CoroPoll cp_;
  std::unique_ptr<ExecutorRpcInterface> rpc_interface_;

  const std::string coordinator_host_;
  const uint16_t coordinator_port_;
};

} // namespace hogd
