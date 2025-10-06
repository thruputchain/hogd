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

#include "coord.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/interface/rpc_interface.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/test_util/test_event_aggregator.h"
#include "x23/rpc.h"
#include "x23/sockio.h"

namespace hogd {

std::unique_ptr<DatabaseProxy>
ExecCoordServer::connect_to_database(DatabaseIdentifier identifier)
{
  return std::make_unique<DatabaseProxy>(std::move(identifier));
}

namespace {
std::unique_ptr<EventServerProxy>
connect_to_event_server(x23::HostPort host_port)
{
  return std::make_unique<EventServerProxy>(std::move(host_port));
}
} // namespace

void
ExecCoordServer::start_rpc_service(DatabaseIdentifier identifier,
                                   x23::HostPort event_addr,
                                   uint16_t self_executor_id)
{
  auto db_proxy = connect_to_database(std::move(identifier));
  auto event_proxy = connect_to_event_server(std::move(event_addr));

  rpc_interface_ =
      std::make_unique<ExecutorRpcInterface>(self_executor_id, cp_,
                                             std::move(db_proxy),
                                             std::move(event_proxy));
}

std::optional<Coordinator1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>
ExecCoordServer::connect_to_coord_allocate_service()
{
  auto fd = cp_.tcpconnect(coord_address_, std::to_string(coord_port_))();
  if (!fd) {
    error("Failed to connect to coord allocate service on {}:{}, error={}",
          coord_address_, coord_port_, fd.error().what());
    return std::nullopt;
  }

  auto client_config = x23::rpc_stream_logged(cp_, *fd);
  return std::make_optional(client_config->template client<Coordinator1>());
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

    return client->coordinator_intro_exec(self_card_)();
  }
}

void
ExecCoordServer::init_from_coord(const LocalMockDB *mock_db)
{
  IntroExecResult exec_result = get_exec_result();
  assert(exec_result.exec_id < std::numeric_limits<uint16_t>::max());

  // TODO(geoff): This won't actually run; but this is pending adding the event
  // server addr to the mempool intro card.
  auto event_port = event_server_exec_rpc_.start_server(0);

  if (mock_db) {
    start_rpc_service(*mock_db, x23::HostPort{"localhost", event_port},
                      exec_result.exec_id);
  }
  else {
    start_rpc_service(ShardedDB(exec_result),
                      x23::HostPort{"localhost", event_port},
                      exec_result.exec_id);
  }
}

} // namespace hogd
