#include <cstdint>
#include <cstdlib>
#include <format>
#include <future>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <sys/socket.h>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/os.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/health/health.h"
#include "hogd/hogexec/interface/executor.h"
#include "hogd/httpd/httpd.h"
#include "hogd/init.h"
#include "x23/sockio.h"

namespace hogd {

namespace {

int
hogexec_init(const std::string &coordinator_addr, const std::string &bind_ip,
             int nthreads, uint16_t mempool_port, uint16_t metrics_port)
{
  Health::instance().start_service();

  httpd::HttpServer metrics_server;
  httpd::add_health_metrics_routes(metrics_server);
  const uint16_t actual_metrics_port =
      metrics_server.start_server(make_bind_host_port(bind_ip, metrics_port));
  info("PORT hogexec.metrics={}", actual_metrics_port);

  x23::HostPort host_port;
  if (!host_port.parse(coordinator_addr)) {
    throw std::runtime_error(
        std::format("Invalid coordinator address: {}", coordinator_addr));
  }
  const std::string coordinator_host = {host_port.host.begin(),
                                        host_port.host.end()};
  const uint16_t coordinator_port = static_cast<uint16_t>(
      std::stoul(std::string{host_port.port.begin(), host_port.port.end()}));

  Executor executor(nthreads);
  const sockaddr_storage coordinator_sockaddr =
      host_port_to_sockaddr(coordinator_host, coordinator_port);
  uint16_t actual_mempool_port =
      executor.start(bind_ip, mempool_port, coordinator_sockaddr);
  CHECK(actual_mempool_port == mempool_port || mempool_port == 0);
  info("PORT hogexec.mempool={}", actual_mempool_port);

  // block forever
  Health::instance().set_status(Health::Status::kOk)();
  info("hogexec initialization complete.");
  std::promise<void>().get_future().wait();

  return EXIT_SUCCESS;
}

} // namespace

} // namespace hogd

namespace {

void
usage(char *argv[])
{
  std::println("usage: {} --coordinator-addr <host:port> --bind-ip <ip> "
               "[--nthreads <nthreads>] [--mempool-port <port>] "
               "[--metrics-port <port>]",
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

} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("starting hogexec!");

  std::string coord_address;
  std::string bind_ip;
  std::optional<int> nthreads;
  uint16_t mempool_port = 0;
  uint16_t metrics_port = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--metrics-port" && i + 1 < argc) {
      metrics_port = parse_port(argv[++i]);
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
      nthreads = std::stoi(argv[++i]);
      continue;
    }
    if (arg == "--mempool-port" && i + 1 < argc) {
      mempool_port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "-n" && i + 1 < argc) {
      nthreads = std::stoi(argv[++i]);
      continue;
    }
    if (arg == "-p" && i + 1 < argc) {
      mempool_port = parse_port(argv[++i]);
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

  return hogd::hogexec_init(coord_address, bind_ip, *nthreads, mempool_port,
                            metrics_port);
}
