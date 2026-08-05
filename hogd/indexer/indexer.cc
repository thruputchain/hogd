#include <cstdint>
#include <cstdlib>
#include <future>
#include <print>
#include <string>

#include "hogd/base/cli.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/rpc_server.h"
#include "hogd/http/http.h"
#include "hogd/indexer/db.h"
#include "hogd/indexer/pull_controller.h"
#include "hogd/indexer/recent_tx_failure_cache.h"
#include "hogd/indexer/rpc_from_block_explorer.h"
#include "hogd/indexer/status.h"
#include "hogd/init.h"
#include "hogd/metrics/shared.h"
#include "network.hh"

namespace hogd {
namespace {

int
indexer_init(const std::string &path, const std::string &event_server_addr,
             const std::string &bind_ip, uint16_t block_explorer_port,
             uint16_t metrics_port)
{
  HttpServer metrics_server;
  add_health_metrics_routes(metrics_server);
  const uint16_t actual_metrics_port =
      metrics_server.start_server(make_bind_host_port(bind_ip, metrics_port));
  info("PORT indexer.metrics={}", actual_metrics_port);

  IndexerStatus<IndexDB> db_status = IndexDB::open_or_make_new(path);
  if (!db_status) {
    panic("[IndexDB::open_or_make_new] failed with {}", db_status.error());
  }
  IndexDB &db = db_status.value();
  Health::instance().set_current_block_height(db.latest_block())();

  RecentTxFailureCache failure_cache;
  IndexerPullController pull_controller(event_server_addr, db, failure_cache);
  pull_controller.wait_until_caught_up();

  // Do not serve stale product queries while startup replay is incomplete.
  RpcServer<IndexerBlockExplorerRpc> block_explorer(db, failure_cache);
  block_explorer->max_message_size_ = kBulkRpcMaxMessageSize;
  uint16_t actual_block_explorer_port = block_explorer.start_server(
      make_bind_host_port(bind_ip, block_explorer_port));
  CHECK(actual_block_explorer_port == block_explorer_port ||
        block_explorer_port == 0);
  info("PORT indexer.block_explorer={}", actual_block_explorer_port);

  Health::instance().set_status(Health::Status::kOk)();

  // The indexer is initialized. Now block forever.
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
               "--bind-ip <ip> [--block-explorer-port <port>] "
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
  if (ret) {
    return ret;
  }
  hogd::info("starting indexer!");

  std::string event_server_addr;
  std::string path;
  std::string bind_ip;
  uint16_t block_explorer_port = 0;
  uint16_t metrics_port = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--metrics-port" && i + 1 < argc) {
      metrics_port =
          hogd::parse_port_arg("--metrics-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--event-addr" && i + 1 < argc) {
      event_server_addr = argv[++i];
      continue;
    }
    if (arg == "--event-port" && i + 1 < argc) {
      hogd::parse_port_arg("--event-port", argv[++i], argv, usage);
      hogd::warn("--event-port is deprecated and ignored");
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
    if (arg == "--block-explorer-port" && i + 1 < argc) {
      block_explorer_port =
          hogd::parse_port_arg("--block-explorer-port", argv[++i], argv, usage);
      continue;
    }
    usage(argv);
  }

  if (event_server_addr.empty() || path.empty() || bind_ip.empty()) {
    usage(argv);
  }
  return hogd::indexer_init(path, event_server_addr, bind_ip,
                            block_explorer_port, metrics_port);
};
