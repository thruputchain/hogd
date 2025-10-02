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

std::unique_ptr<DatabaseProxy>
ExecCoordServer::connect_to_database(DatabaseIdentifier identifier)
{
  return std::make_unique<DatabaseProxy>(std::move(identifier));
}

void
ExecCoordServer::start_rpc_service(DatabaseIdentifier identifier,
                                   uint16_t self_executor_id)
{
  auto db_proxy = connect_to_database(std::move(identifier));
  rpc_interface_->init(self_executor_id, std::move(db_proxy));
}

std::optional<SchedExec1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>
ExecCoordServer::connect_to_coord_allocate_service()
{
  auto fd = cp_.tcpconnect(coord_address_, std::to_string(coord_port_))();
  if (!fd) {
    error("Failed to connect to coord allocate service on {}:{}, error={}",
          coord_address_, coord_port_, fd.error().what());
    return std::nullopt;
  }

  auto client_config = x23::rpc_stream_logged(cp_, *fd);
  return std::make_optional(client_config->template client<SchedExec1>());
}

IntroExecResult
ExecCoordServer::get_exec_result()
{
  while (true) {
    auto client = connect_to_coord_allocate_service();
    if (!client) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    ExecCard card = {.protocol = version::kHogdProtocol,
                     .upid = {},
                     .address = get_executor_address().c_str()};
    auto ret = client->schedexec_intro_exec(card)();
    return ret;
  }
}

void
ExecCoordServer::init_from_coord()
{
  IntroExecResult exec_result = get_exec_result();
  assert(exec_result.exec_id < std::numeric_limits<uint16_t>::max());
  start_rpc_service(ShardedDB(exec_result), exec_result.exec_id);
}

} // namespace hogd
