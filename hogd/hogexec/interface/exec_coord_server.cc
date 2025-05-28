#include "hogd/hogexec/interface/exec_coord_server.h"

#include <cassert>
#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "coord.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/interface/rpc_interface.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "x23/rpc.h"

namespace hogd {

std::unique_ptr<DatabaseProxy>
ExecCoordServer::connect_to_database(DatabaseIdentifier identifier)
{
  return std::make_unique<DatabaseProxy>(cp_, std::move(identifier));
}

void
ExecCoordServer::start_rpc_service(DatabaseIdentifier identifier,
                                   uint16_t self_executor_id)
{
  auto db_proxy = connect_to_database(std::move(identifier));
  rpc_interface_ = std::make_unique<ExecutorRpcInterface>(self_executor_id,
                                                          main_worker_pool_,
                                                          std::move(db_proxy));
}

std::optional<Coordinator1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>
ExecCoordServer::connect_to_coord_allocate_service(
    std::string const &coord_address, std::string const &coord_allocate_port)
{
  auto fd = cp_.tcpconnect(coord_address, coord_allocate_port)();
  if (!fd) {
    return std::nullopt;
  }

  auto client_config = x23::rpc_stream_logged(cp_, *fd);
  return std::make_optional(client_config.template client<Coordinator1>());
}

IntroExecResult
ExecCoordServer::get_exec_result(std::string const &coord_address,
                                 std::string const &coord_allocate_port)
{
  while (true) {
    auto client =
        connect_to_coord_allocate_service(coord_address, coord_allocate_port);
    if (!client) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    return client->coordinator_intro_exec(self_card_)();
  }
}

void
ExecCoordServer::init_from_coord(std::string const &coord_address,
                                 std::string const &coord_allocate_port)
{
  auto exec_result = get_exec_result(coord_address, coord_allocate_port);
  assert(exec_result.exec_id < std::numeric_limits<uint16_t>::max());
  start_rpc_service(ShardedDB(exec_result), exec_result.exec_id);
}

} // namespace hogd
