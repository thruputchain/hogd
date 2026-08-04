#include <cstdint>
#include <cstdlib>
#include <format>
#include <future>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <sys/socket.h>

#include "hogd/base/cli.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/os.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/interface/executor.h"
#include "hogd/hog_exec/interface/rpc_from_view_gateway.h"
#include "hogd/hog_exec/shared.h"
#include "hogd/http/http.h"
#include "hogd/init.h"
#include "hogd/metrics/shared.h"
#include "x23/sockio.h"

namespace hogd {

namespace {

struct ParsedHostPort {
  std::string host;
  uint16_t port;
};

ParsedHostPort
parse_host_port(const std::string &label, const std::string &address)
{
  x23::HostPort host_port;
  const std::optional<uint16_t> port =
      host_port.parse(address)
          ? try_parse_port(
                std::string_view{host_port.port.begin(), host_port.port.end()})
          : std::nullopt;
  if (!port) {
    panic("Invalid {}: {}", label, address);
  }
  return ParsedHostPort{.host = std::string{host_port.host.begin(),
                                            host_port.host.end()},
                        .port = *port};
}

int
hog_exec_init(const std::string &coordinator_addr, const std::string &bind_ip,
             int nthreads, uint16_t mempool_port, uint16_t metrics_port,
             const std::optional<std::string> &db_executor_addr,
             const std::optional<std::string> &event_executor_addr,
             std::optional<uint16_t> view_gateway_port)
{
  init_hog_exec_metrics();

  HttpServer metrics_server;
  add_health_metrics_routes(metrics_server);
  const uint16_t actual_metrics_port =
      metrics_server.start_server(make_bind_host_port(bind_ip, metrics_port));
  info("PORT hog_exec.metrics={}", actual_metrics_port);

  const ParsedHostPort coordinator =
      parse_host_port("coordinator address", coordinator_addr);

  const bool view_gateway_configured =
      db_executor_addr || event_executor_addr || view_gateway_port;
  if (view_gateway_configured &&
      (!db_executor_addr || !event_executor_addr || !view_gateway_port)) {
    panic("--db-executor-addr, --event-executor-addr, and --view-gateway-port "
          "must be provided together");
  }

  Executor executor(nthreads);
  const sockaddr_storage coordinator_sockaddr =
      host_port_to_sockaddr(coordinator.host, coordinator.port);
  uint16_t actual_mempool_port =
      executor.start(bind_ip, mempool_port, coordinator_sockaddr);
  CHECK(actual_mempool_port == mempool_port || mempool_port == 0);
  info("PORT hog_exec.mempool={}", actual_mempool_port);

  std::unique_ptr<ViewGatewayServer> view_gateway_server;
  if (view_gateway_configured) {
    const ParsedHostPort event_executor =
        parse_host_port("event executor address", *event_executor_addr);
    view_gateway_server =
        std::make_unique<ViewGatewayServer>(*db_executor_addr,
                                            event_executor.host,
                                            event_executor.port);
    const uint16_t actual_view_gateway_port = view_gateway_server->start_server(
        make_bind_host_port(bind_ip, *view_gateway_port));
    CHECK(actual_view_gateway_port == *view_gateway_port ||
          *view_gateway_port == 0);
    info("PORT hog_exec.view_gateway={}", actual_view_gateway_port);
  }

  // block forever
  Health::instance().set_status(Health::Status::kOk)();
  info("hog_exec initialization complete.");
  std::promise<void>().get_future().wait();

  return EXIT_SUCCESS;
}

} // namespace

} // namespace hogd

namespace {

[[noreturn]] void
usage(char *argv[])
{
  std::println("usage: {} --coordinator-addr <host:port> --bind-ip <ip> "
               "[--nthreads <nthreads>] [--mempool-port <port>] "
               "[--metrics-port <port>] "
               "[--db-executor-addr <host:port> "
               "--event-executor-addr <host:port> "
               "--view-gateway-port <port>] [--version]",
               argv[0]);
  exit(EXIT_FAILURE);
}

int
parse_nthreads(char *argv[], const std::string &value)
{
  const std::optional<int> nthreads = hogd::try_parse_integer<int>(value);
  if (!nthreads || *nthreads <= 0) {
    std::println(stderr, "Invalid --nthreads: {}", value);
    usage(argv);
  }
  return *nthreads;
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
  hogd::info("starting hog_exec!");

  std::string coord_address;
  std::string bind_ip;
  std::optional<int> nthreads;
  uint16_t mempool_port = 0;
  uint16_t metrics_port = 0;
  std::optional<std::string> db_executor_addr;
  std::optional<std::string> event_executor_addr;
  std::optional<uint16_t> view_gateway_port;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--metrics-port" && i + 1 < argc) {
      metrics_port =
          hogd::parse_port_arg("--metrics-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--coordinator-addr" && i + 1 < argc) {
      coord_address = argv[++i];
      continue;
    }
    if (arg == "--bind-ip" && i + 1 < argc) {
      bind_ip = argv[++i];
      continue;
    }
    if (arg == "--nthreads" && i + 1 < argc) {
      nthreads = parse_nthreads(argv, argv[++i]);
      continue;
    }
    if (arg == "--mempool-port" && i + 1 < argc) {
      mempool_port =
          hogd::parse_port_arg("--mempool-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--db-executor-addr" && i + 1 < argc) {
      db_executor_addr = argv[++i];
      continue;
    }
    if (arg == "--event-executor-addr" && i + 1 < argc) {
      event_executor_addr = argv[++i];
      continue;
    }
    if (arg == "--view-gateway-port" && i + 1 < argc) {
      view_gateway_port =
          hogd::parse_port_arg("--view-gateway-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "-n" && i + 1 < argc) {
      nthreads = parse_nthreads(argv, argv[++i]);
      continue;
    }
    if (arg == "-p" && i + 1 < argc) {
      mempool_port = hogd::parse_port_arg("-p", argv[++i], argv, usage);
      continue;
    }
    if (arg == "-h" || arg == "--help") {
      usage(argv);
    }
    usage(argv);
  }

  if (!nthreads) {
    nthreads = hogd::available_parallelism();
  }

  if (coord_address.empty() || bind_ip.empty()) {
    usage(argv);
  }

  return hogd::hog_exec_init(coord_address, bind_ip, *nthreads, mempool_port,
                            metrics_port, db_executor_addr, event_executor_addr,
                            view_gateway_port);
}
