#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <future>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/health/health.h"
#include "hogd/httpd/httpd.h"
#include "hogd/init.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/mempool/registration.h"
#include "hogd/mempool/rpc_from_auth.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/rpc_from_db.h"
#include "hogd/mempool/rpc_from_event.h"
#include "hogd/mempool/rpc_from_exec.h"
#include "hogd/mempool/rpc_from_gateway.h"
#include "x23/async.h"
#include "x23/sockio.h"

namespace hogd {

namespace {

int
mempool_init(size_t num_db_shards, size_t num_exec_shards, uint64_t network_id,
             std::string_view bind_ip, uint16_t db_port, uint16_t event_port,
             uint16_t executor_port, uint16_t auth_port,
             uint16_t coordinator_port, uint16_t gateway_port,
             std::optional<x23::HostPort> metrics_addr)
{
  Registration &registration = Registration::registration();

  info("NETWORK ID={:#x}", network_id);

  // Start the health service.
  Health::instance().start_service();

  std::optional<httpd::HttpServer> metrics_server;
  if (metrics_addr.has_value()) {
    metrics_server.emplace();
    metrics_server->add_route(
        "/health", []() -> x23::Async<httpd::HttpResponse> {
          co_return httpd::HttpResponse{
              .status = 200,
              .content_type = "application/json",
              .body = co_await Health::instance().dump_to_string(),
          };
        });
    metrics_server->add_route("/metrics", [] {
      return httpd::HttpResponse{
          .status = 200,
          .content_type = "text/plain",
          .body = "",
      };
    });
    const uint16_t actual_metrics_port =
        metrics_server->start_server(std::move(*metrics_addr));
    info("PORT mempool.metrics={}", actual_metrics_port);
  }

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
  auth->max_message_size_ = LoadgenRpc::kRpcMaxMessageSize;
  uint16_t actual_auth_port =
      auth.start_server(make_bind_host_port(bind_ip, auth_port));
  CHECK(actual_auth_port == auth_port || auth_port == 0);
  info("PORT mempool.auth={}", actual_auth_port);
  while (!registration.auth_registered(/*card=*/nullptr)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // TODO(jhumphri): Make [mempool_tx_size_limit] a command line arg.
  RpcServer<MempoolCoordinatorRpc> coord(
      db->impl(), auth->impl(), network_id,
      MempoolCoordinatorRpc::kDefaultMempoolTxSizeLimit);
  coord->max_message_size_ = LoadgenRpc::kRpcMaxMessageSize;
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
  gateway->max_message_size_ = LoadgenRpc::kRpcMaxMessageSize;
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
      "--network-id <hex> [--db-port <port>] [--event-port <port>] "
      "[--executor-port <port>] [--auth-port <port>] "
      "[--coordinator-port <port>] [--gateway-port <port>] "
      "[--metrics=<private-ip:port>]",
      argv[0]);
  exit(EXIT_FAILURE);
}

uint16_t
parse_port(const std::string &value)
{
  size_t pos = 0;
  uint64_t port = std::stoul(value, &pos, 10);
  if (pos != value.size() || port > 65535) {
    throw std::runtime_error(std::format("Invalid port: {}", value));
  }
  return static_cast<uint16_t>(port);
}

std::optional<x23::HostPort>
parse_metrics_addr(const std::string &arg)
{
  constexpr std::string_view kPrefix = "--metrics=";
  if (!arg.starts_with(kPrefix)) {
    return std::nullopt;
  }
  x23::HostPort host_port;
  CHECK(host_port.parse(std::string_view(arg).substr(kPrefix.size())));
  std::expected<sockaddr_storage, x23::GaiError> addr =
      host_port.to_addr(/*dns=*/false);
  CHECK(addr.has_value());
  return host_port;
}

} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("starting mempool!");

  std::string bind_ip;
  std::optional<size_t> num_db_shards;
  std::optional<size_t> num_exec_shards;
  std::optional<uint64_t> network_id;
  uint16_t db_port = 0;
  uint16_t event_port = 0;
  uint16_t executor_port = 0;
  uint16_t auth_port = 0;
  uint16_t coordinator_port = 0;
  uint16_t gateway_port = 0;
  std::optional<x23::HostPort> metrics_addr;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (std::optional<x23::HostPort> parsed_metrics_addr =
            parse_metrics_addr(arg)) {
      metrics_addr = std::move(*parsed_metrics_addr);
      continue;
    }
    if (arg == "--bind-ip" && i + 1 < argc) {
      bind_ip = argv[++i];
      continue;
    }
    if (arg == "--num-db-shards" && i + 1 < argc) {
      num_db_shards = std::stoul(argv[++i], nullptr, /*base=*/10);
      continue;
    }
    if (arg == "--num-exec-shards" && i + 1 < argc) {
      num_exec_shards = std::stoul(argv[++i], nullptr, /*base=*/10);
      continue;
    }
    if (arg == "--network-id" && i + 1 < argc) {
      network_id = std::stoul(argv[++i], nullptr, /*base=*/16);
      continue;
    }
    if (arg == "--db-port" && i + 1 < argc) {
      db_port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--event-port" && i + 1 < argc) {
      event_port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--executor-port" && i + 1 < argc) {
      executor_port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--auth-port" && i + 1 < argc) {
      auth_port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--coordinator-port" && i + 1 < argc) {
      coordinator_port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--gateway-port" && i + 1 < argc) {
      gateway_port = parse_port(argv[++i]);
      continue;
    }
    usage(argv);
  }

  if (bind_ip.empty() || !num_db_shards.has_value() ||
      !num_exec_shards.has_value() || !network_id.has_value()) {
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

  return hogd::mempool_init(*num_db_shards, *num_exec_shards, *network_id,
                            bind_ip, db_port, event_port, executor_port,
                            auth_port, coordinator_port, gateway_port,
                            std::move(metrics_addr));
}
