#include "hogd/hogexec/interface/executor.h"

#include <cassert>
#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/version.h"
#include "sched_exec.hh"
#include "x23/cleanup.h"
#include "x23/rpc.h"
#include "x23/sockio.h"

namespace hogd {

std::string
MempoolExecutorClient::get_executor_address(std::string_view hostname,
                                            uint16_t executor_port)
{
  return std::format("{}:{}", hostname, executor_port);
}

SchedExec1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>
MempoolExecutorClient::connect_to_coordinator()
{
  sockaddr_storage coordinator_sockaddr =
      host_port_to_sockaddr(coordinator_host_, coordinator_port_);
  while (true) {
    x23::UniqueFd fd = x23::tcpconnect(coordinator_sockaddr);
    if (fd) {
      return x23::rpc_stream_logged(cp_, fd.release())->client(SchedExec1{});
    }
    error("Failed to connect to coord allocate service on {}:{}",
          coordinator_host_, coordinator_port_);
    // Wait briefly and then try again on the next iteration...
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

IntroExecResult
MempoolExecutorClient::register_with_coordinator(std::string hostname,
                                                 uint16_t executor_port)
{
  if (hostname.empty()) {
    CHECK_EQ(get_hostname(hostname), 0);
  }
  auto client = connect_to_coordinator();
  const ExecCard card = {
      .protocol = version::kHogdProtocol,
      .upid = {},
      .address = get_executor_address(hostname, executor_port).c_str()};
  auto ret = client.schedexec_intro_exec(card)();
  return ret;
}

} // namespace hogd
