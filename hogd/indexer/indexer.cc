#include <cstdint>
#include <cstdlib>
#include <format>
#include <future>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/health/health.h"
#include "hogd/httpd/httpd.h"
#include "hogd/indexer/db.h"
#include "hogd/indexer/rpc_from_block_explorer.h"
#include "hogd/indexer/rpc_from_event.h"
#include "hogd/indexer/shared.h"
#include "hogd/indexer/status.h"
#include "hogd/init.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/version.h"
#include "indexer.hh"

namespace hogd {

namespace {

int
indexer_init(const std::string &path, const std::string &event_server_addr,
             const std::string &bind_ip, uint16_t event_port,
             uint16_t block_explorer_port,
             std::optional<x23::HostPort> metrics_addr)
{
  Health::instance().start_service();

  std::optional<httpd::HttpServer> metrics_server;
  if (metrics_addr.has_value()) {
    metrics_server.emplace();
    httpd::add_health_metrics_routes(*metrics_server);
    const uint16_t actual_metrics_port =
        metrics_server->start_server(std::move(*metrics_addr));
    info("PORT indexer.metrics={}", actual_metrics_port);
  }

  IndexerStatus<IndexDB> db_status = IndexDB::open_or_make_new(path);
  if (!db_status) {
    panic("[IndexDB::open_or_make_new] failed with {}", db_status.error());
  }
  IndexDB &db = db_status.value();
  Health::instance().set_current_block_height(db.latest_block())();

  // Create the RPC server that the event server talks to.
  RpcServer<IndexerEventRpc> event(db);
  event->max_message_size_ = LoadgenRpc::kRpcMaxMessageSize;
  uint16_t actual_event_port =
      event.start_server(make_bind_host_port(bind_ip, event_port));
  CHECK(actual_event_port == event_port || event_port == 0);
  info("PORT indexer.event={}", actual_event_port);

  // Register with the event server.
  event_indexer_client_t client = make_rpc_client<EventIndexer1>(
      server_address_to_sockaddr(event_server_addr), /*retry=*/true)();
  const std::string indexer_server_addr =
      format_advertised_address(bind_ip, /*port=*/actual_event_port);
  const IndexerCard card{.protocol = version::kHogdProtocol(),
                         .address_for_event = indexer_server_addr.c_str()};
  client.event_indexer_intro(card)();

  // Create the RPC server that the block explorers talk to.
  RpcServer<IndexerBlockExplorerRpc> block_explorer(db);
  block_explorer->max_message_size_ = LoadgenRpc::kRpcMaxMessageSize;
  uint16_t actual_block_explorer_port = block_explorer.start_server(
      make_bind_host_port(bind_ip, block_explorer_port));
  CHECK(actual_block_explorer_port == block_explorer_port ||
        block_explorer_port == 0);
  info("PORT indexer.block_explorer={}", actual_block_explorer_port);

  // The indexer is initialized. Now block forever.
  Health::instance().set_status(Health::Status::kOk)();
  info("indexer initialization complete.");
  std::promise<void>().get_future().wait();

  return 0;
}

} // namespace

} // namespace hogd

namespace {

[[noreturn]] void
usage(char *argv[])
{
  std::println("usage: {} --event-addr <host:port> --db-path <path> "
               "--bind-ip <ip> [--event-port <port>] "
               "[--block-explorer-port <port>] "
               "[--metrics <private-ip:port>]",
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
parse_metrics_addr(std::string_view value)
{
  x23::HostPort host_port;
  CHECK(host_port.parse(value));
  CHECK(host_port.to_addr(/*dns=*/false).has_value());
  return host_port;
}

} // namespace

// NOLINTBEGIN(bugprone-exception-escape)
int
main(int argc, char *argv[])
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret) {
    return ret;
  }
  hogd::info("starting indexer!");

  std::string event_server_addr;
  std::string path;
  std::string bind_ip;
  uint16_t event_port = 0;
  uint16_t block_explorer_port = 0;
  std::optional<x23::HostPort> metrics_addr;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--metrics" && i + 1 < argc) {
      metrics_addr = parse_metrics_addr(argv[++i]);
      continue;
    }
    if (arg == "--event-addr" && i + 1 < argc) {
      event_server_addr = argv[++i];
      continue;
    }
    if (arg == "--db-path" && i + 1 < argc) {
      path = argv[++i];
      continue;
    }
    if (arg == "--bind-ip" && i + 1 < argc) {
      bind_ip = argv[++i];
      continue;
    }
    if (arg == "--event-port" && i + 1 < argc) {
      event_port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--block-explorer-port" && i + 1 < argc) {
      block_explorer_port = parse_port(argv[++i]);
      continue;
    }
    usage(argv);
  }

  if (event_server_addr.empty() || path.empty() || bind_ip.empty()) {
    usage(argv);
  }
  return hogd::indexer_init(path, event_server_addr, bind_ip, event_port,
                            block_explorer_port, std::move(metrics_addr));
};

// NOLINTEND(bugprone-exception-escape)
