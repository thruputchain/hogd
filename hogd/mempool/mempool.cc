#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <future>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "hogd/base/cli.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/network_hash.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/http/http.h"
#include "hogd/init.h"
#include "hogd/mempool/registration.h"
#include "hogd/mempool/rpc_from_auth.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/rpc_from_db.h"
#include "hogd/mempool/rpc_from_event.h"
#include "hogd/mempool/rpc_from_exec.h"
#include "hogd/mempool/rpc_from_gateway.h"
#include "hogd/metrics/shared.h"
#include "types.hh"

namespace hogd {

namespace {

int
mempool_init(size_t num_db_shards, size_t num_exec_shards,
             NetworkHash network_hash, std::string_view bind_ip,
             uint16_t db_port, uint16_t event_port, uint16_t executor_port,
             uint16_t auth_port, uint16_t coordinator_port,
             uint16_t gateway_port, uint16_t metrics_port)
{
  Registration &registration = Registration::registration();

  info("NETWORK HASH={}", format_network_hash(network_hash));

  // We finish setting descriptions for all metrics *before* we start the HTTP
  // server. The HTTP server reads the descriptions, so we want to avoid a race.
  HttpServer metrics_server;
  add_health_metrics_routes(metrics_server);
  const uint16_t actual_metrics_port =
      metrics_server.start_server(make_bind_host_port(bind_ip, metrics_port));
  info("PORT mempool.metrics={}", actual_metrics_port);

  // Wait for the db shards to connect.
  RpcServer<MempoolDbRpc> db;
  uint16_t actual_db_port =
      db.start_server(make_bind_host_port(bind_ip, db_port));
  CHECK(actual_db_port == db_port || db_port == 0);
  info("PORT mempool.db={}", actual_db_port);
  info("Waiting for {} db shards to connect...", num_db_shards);
  while (registration.shards_registered(/*cards=*/nullptr) < num_db_shards) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Wait for the event subsystem to connect.
  RpcServer<MempoolEventRpc> event;
  uint16_t actual_event_port =
      event.start_server(make_bind_host_port(bind_ip, event_port));
  CHECK(actual_event_port == event_port || event_port == 0);
  info("PORT mempool.event={}", actual_event_port);
  info("Waiting for event server to connect...");
  while (!registration.event_registered(/*card=*/nullptr)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Let the executors connect.
  RpcServer<MempoolExecutorRpc> exec;
  uint16_t actual_executor_port =
      exec.start_server(make_bind_host_port(bind_ip, executor_port));
  CHECK(actual_executor_port == executor_port || executor_port == 0);
  info("PORT mempool.executor={}", actual_executor_port);
  info("Waiting for {} executors to connect...", num_exec_shards);
  while (registration.executors_registered(/*cards=*/nullptr) <
         num_exec_shards) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  RpcServer<MempoolAuthRpc> auth;
  auth->max_message_size_ = kBulkRpcMaxMessageSize;
  uint16_t actual_auth_port =
      auth.start_server(make_bind_host_port(bind_ip, auth_port));
  CHECK(actual_auth_port == auth_port || auth_port == 0);
  info("PORT mempool.auth={}", actual_auth_port);
  while (!registration.auth_registered(/*card=*/nullptr)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // TODO(jhumphri): Make [mempool_tx_size_limit] a command line arg.
  RpcServer<MempoolCoordinatorRpc> coord(
      db->impl(), auth->impl(), std::move(network_hash),
      MempoolCoordinatorRpc::kDefaultMempoolTxSizeLimit);
  coord->max_message_size_ = kBulkRpcMaxMessageSize;
  uint16_t actual_coordinator_port =
      coord.start_server(make_bind_host_port(bind_ip, coordinator_port));
  CHECK(actual_coordinator_port == coordinator_port || coordinator_port == 0);
  info("PORT mempool.coordinator={}", actual_coordinator_port);

  while (!coord->impl()->ready()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Public RPC gateway server: shares the scheduler + admission path
  // with the coordinator server, but exposes a gateway-shaped surface
  // (SchedGateway1) on a separate port.  Chuckd remains on the
  // coordinator port; the public RPC gateway binds to this one.
  MempoolGatewayServer gateway(coord->impl());
  gateway->max_message_size_ = kBulkRpcMaxMessageSize;
  uint16_t actual_gateway_port =
      gateway.start_server(make_bind_host_port(bind_ip, gateway_port));
  CHECK(actual_gateway_port == gateway_port || gateway_port == 0);
  info("PORT mempool.gateway={}", actual_gateway_port);

  // The mempool is initialized. Now block forever.
  info("mempool initialization complete.");
  std::promise<void>().get_future().wait();

  // TODO(jhumphri): Implement shutdown path.
  // db.impl()->reset_scheduler()();
  // coord.shut_down();
  // exec.shut_down();
  // db.shut_down();

  return 0;
}

} // namespace

} // namespace hogd

namespace {

[[noreturn]] void
usage(char *argv[])
{
  std::println(
      "usage: {} --bind-ip <ip> --num-db-shards <n> --num-exec-shards <n> "
      "--network-hash <64 hex digits> [--db-port <port>] [--event-port <port>] "
      "[--executor-port <port>] [--auth-port <port>] "
      "[--coordinator-port <port>] [--gateway-port <port>] "
      "[--metrics-port <port>] [--version]",
      argv[0]);
  exit(EXIT_FAILURE);
}

} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  if (hogd::print_version_and_exit_if_requested(argc, argv)) {
    return EXIT_SUCCESS;
  }
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("starting mempool!");

  std::string bind_ip;
  std::optional<size_t> num_db_shards;
  std::optional<size_t> num_exec_shards;
  std::optional<hogd::NetworkHash> network_hash;
  uint16_t db_port = 0;
  uint16_t event_port = 0;
  uint16_t executor_port = 0;
  uint16_t auth_port = 0;
  uint16_t coordinator_port = 0;
  uint16_t gateway_port = 0;
  uint16_t metrics_port = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--metrics-port" && i + 1 < argc) {
      metrics_port =
          hogd::parse_port_arg("--metrics-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--bind-ip" && i + 1 < argc) {
      bind_ip = argv[++i];
      continue;
    }
    if (arg == "--num-db-shards" && i + 1 < argc) {
      num_db_shards = hogd::parse_integer_arg<size_t>("--num-db-shards",
                                                      argv[++i], argv, usage);
      continue;
    }
    if (arg == "--num-exec-shards" && i + 1 < argc) {
      num_exec_shards = hogd::parse_integer_arg<size_t>("--num-exec-shards",
                                                        argv[++i], argv, usage);
      continue;
    }
    if (arg == "--network-hash" && i + 1 < argc) {
      network_hash = hogd::parse_network_hash(argv[++i]);
      if (!network_hash) {
        usage(argv);
      }
      continue;
    }
    if (arg == "--db-port" && i + 1 < argc) {
      db_port = hogd::parse_port_arg("--db-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--event-port" && i + 1 < argc) {
      event_port = hogd::parse_port_arg("--event-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--executor-port" && i + 1 < argc) {
      executor_port =
          hogd::parse_port_arg("--executor-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--auth-port" && i + 1 < argc) {
      auth_port = hogd::parse_port_arg("--auth-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--coordinator-port" && i + 1 < argc) {
      coordinator_port =
          hogd::parse_port_arg("--coordinator-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--gateway-port" && i + 1 < argc) {
      gateway_port =
          hogd::parse_port_arg("--gateway-port", argv[++i], argv, usage);
      continue;
    }
    usage(argv);
  }

  if (bind_ip.empty() || !num_db_shards.has_value() ||
      !num_exec_shards.has_value() || !network_hash.has_value()) {
    usage(argv);
  }

  if (*num_db_shards == 0) {
    std::println("[num_db_shards] must be greater than 0.");
    return EXIT_FAILURE;
  }

  if (*num_exec_shards == 0) {
    std::println("[num_exec_shards] must be greater than 0.");
    return EXIT_FAILURE;
  }

  return hogd::mempool_init(*num_db_shards, *num_exec_shards,
                            std::move(*network_hash), bind_ip, db_port,
                            event_port, executor_port, auth_port,
                            coordinator_port, gateway_port, metrics_port);
}
