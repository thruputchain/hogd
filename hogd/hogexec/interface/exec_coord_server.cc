#include "hogd/hogexec/interface/exec_coord_server.h"

#include <cassert>
#include <chrono>
#include <format>
#include <limits> // NOLINT(misc-include-cleaner)
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/interface/rpc_interface.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/version.h"
#include "sched_exec.hh"
#include "x23/rpc.h"

namespace hogd {

std::string
MempoolExecutorClient::get_executor_address(uint16_t executor_port)
{
  std::string hostname;
  CHECK_EQ(get_hostname(hostname), 0);
  return std::format("{}:{}", hostname, executor_port);
}

std::optional<SchedExec1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>
MempoolExecutorClient::connect_to_coordinator()
{
  // TODO(jhumphri): Take these out!!!
  static x23::ThreadPool pool;
  static x23::CoroPoll cp(pool);
  while (true) {
    auto fd =
        cp.tcpconnect(coordinator_host_, std::to_string(coordinator_port_))();
    if (fd) {
      auto client_config = x23::rpc_stream_logged(cp, *fd);
      return std::make_optional(client_config->template client<SchedExec1>());
    }
    error("Failed to connect to coord allocate service on {}:{}, error={}",
          coordinator_host_, coordinator_port_, fd.error().what());
    // Wait briefly and then try again on the next iteration...
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

IntroExecResult
MempoolExecutorClient::register_with_coordinator(uint16_t executor_port)
{
  auto client = connect_to_coordinator();
  const ExecCard card = {.protocol = version::kHogdProtocol,
                         .upid = {},
                         .address =
                             get_executor_address(executor_port).c_str()};
  auto ret = client->schedexec_intro_exec(card)();
  return ret;
}

} // namespace hogd
