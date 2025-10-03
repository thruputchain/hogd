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
ExecCoordServer::get_executor_address() const
{
  std::string hostname;
  CHECK_EQ(get_hostname(hostname), 0);
  return std::format("{}:{}", hostname, rpc_interface_->port());
}

std::optional<SchedExec1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>
ExecCoordServer::connect_to_coordinator()
{
  while (true) {
    auto fd =
        cp_.tcpconnect(coordinator_host_, std::to_string(coordinator_port_))();
    if (fd) {
      auto client_config = x23::rpc_stream_logged(cp_, *fd);
      return std::make_optional(client_config->template client<SchedExec1>());
    }
    error("Failed to connect to coord allocate service on {}:{}, error={}",
          coordinator_host_, coordinator_port_, fd.error().what());
    // Wait briefly and then try again on the next iteration...
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

IntroExecResult
ExecCoordServer::register_with_coordinator()
{
  auto client = connect_to_coordinator();
  const ExecCard card = {.protocol = version::kHogdProtocol,
                         .upid = {},
                         .address = get_executor_address().c_str()};
  return client->schedexec_intro_exec(card)();
}

void
ExecCoordServer::start_rpc_service(DatabaseIdentifier identifier,
                                   uint16_t self_executor_id)
{
  auto db_proxy = std::make_unique<DatabaseProxy>(std::move(identifier));
  rpc_interface_->init(self_executor_id, std::move(db_proxy));
}

} // namespace hogd
