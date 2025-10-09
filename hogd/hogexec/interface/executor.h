#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/interface/rpc_from_coord.h"
#include "sched_exec.hh"
#include "x23/event.h"
#include "x23/rpc.h"

namespace hogd {

class MempoolExecutorClient {
public:
  MempoolExecutorClient(x23::CoroPoll &cp, std::string coordinator_host,
                        uint16_t coordinator_port)
    : cp_(cp), coordinator_host_(std::move(coordinator_host)),
      coordinator_port_(coordinator_port)
  {}

  // Register this executor with the coordinator.
  IntroExecResult register_with_coordinator(std::string hostname,
                                            uint16_t executor_port);

private:
  // Generates the address of this executor, which includes both the hostname
  // and the port of the executor RPC server. We send this address to the
  // coordinator when we register this executor with it.
  static std::string get_executor_address(std::string_view hostname,
                                          uint16_t executor_port);

  // Create an RPC client that is connected to the coordinator.
  SchedExec1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>
  connect_to_coordinator();

  x23::CoroPoll &cp_;
  const std::string coordinator_host_;
  const uint16_t coordinator_port_;
};

// Server that handles RPC communication between the executor and coordinator.
// Constructor connects to the coordinator to receive information about the
// database, then connects to the database and begins serving RPCs to the
// coordinator.
class Executor {
public:
  Executor(std::string coordinator_host, uint16_t coordinator_port)
    : coordinator_host_(std::move(std::move(coordinator_host))),
      coordinator_port_(coordinator_port)
  {}

  ~Executor() { await_shutdown_request(); }

  uint16_t start(uint16_t port_number, std::string hostname = "")
  {
    // When we register with the coordinator, we need to tell the coordinator
    // our port number. However, we do not know our port number until we
    // actually start our server (especially because [port_number] could be 0,
    // in which case the kernel will assign a port number to us).
    // Thus, we start the RPC server first, then below we register with the
    // coordinator, and finally we start handling incoming RPCs in
    // [start_rpc_service()].
    uint16_t executor_port = server_.start_server(port_number);

    // Now register with the coordinator.
    MempoolExecutorClient client(server_.cp_, coordinator_host_,
                                 coordinator_port_);
    IntroExecResult exec_result =
        client.register_with_coordinator(std::move(hostname), executor_port);
    // Connects to the database and performs initialization of the executor.
    // Then begins serving RPCs to the coordinator.
    server_.impl_.init(std::move(exec_result));

    return executor_port;
  }

  void shut_down() { server_.shut_down(); }
  void await_shutdown_request() { server_.impl_.await_shutdown_request(); }

private:
  RpcServer<ExecutorMempoolRpc> server_;

  const std::string coordinator_host_;
  const uint16_t coordinator_port_;
};

} // namespace hogd
