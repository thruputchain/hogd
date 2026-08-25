#pragma once

#include <cassert>
#include <string>
#include <sys/socket.h>
#include <utility>

#include "hogd/base/network.h"
#include "hogd/base/os.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/interface/rpc_from_coord.h"
#include "sched_exec.hh"
#include "x23/sockio.h"

namespace hogd {

class MempoolExecutorClient {
public:
  // Register this executor with the coordinator.
  static IntroExecResult
  register_with_coordinator(x23::HostPort const &self_hostport,
                            const sockaddr_storage &coordinator_sockaddr);
};

// Server that handles RPC communication between the executor and coordinator.
// Constructor connects to the coordinator to receive information about the
// database, then connects to the database and begins serving RPCs to the
// coordinator.
class Executor {
public:
  // ONC RPC single-fragment record limit; x23 cannot frame a larger message.
  static constexpr size_t kMessageSize = 0x7fffffff;

  Executor() = default;
  ~Executor() { await_shutdown_request(); }

  uint16_t start(std::string const &bind_ip, uint16_t self_port,
                 const sockaddr_storage &coordinator_sockaddr)
  {
    // When we register with the coordinator, we need to tell the coordinator
    // our port number. However, we do not know our port number until we
    // actually bind our server socket (especially because [self_port] could be
    // 0, in which case the kernel will assign a port number to us).
    //
    // We must NOT start handling incoming RPCs until init() has finished
    // publishing executor_states_/self_executor_id_, or coordinator RPCs would
    // race that initialization (the handlers read those members without
    // synchronization). So we split startup into three sequenced steps: bind to
    // learn the port, register + init, and only then start accepting. While
    // init() runs, the kernel queues any coordinator connections/requests in
    // the listen backlog; we drain them once start_accepting() is called.
    server_->max_message_size_ = kMessageSize;
    auto [executor_port, listen_fd] =
        server_.bind_server(make_bind_host_port(bind_ip, self_port));
    assert(executor_port == self_port || self_port == 0);

    // Now register with the coordinator.
    IntroExecResult exec_result =
        MempoolExecutorClient::register_with_coordinator(
            make_bind_host_port(bind_ip, executor_port), coordinator_sockaddr);
    // Connects to the database and performs initialization of the executor.
    server_->impl()->init(std::move(exec_result));

    // init() has published all state the RPC handlers read; it is now safe to
    // begin serving RPCs to the coordinator.
    server_.start_accepting(std::move(listen_fd));

    return executor_port;
  }

  void await_shutdown_request() { server_->impl()->await_shutdown_request(); }

  void shut_down() { server_->impl()->shut_down(); }

private:
  RpcServer<ExecutorMempoolRpc> server_;
};

} // namespace hogd
