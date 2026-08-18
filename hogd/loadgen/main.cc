#include <cstdint>
#include <cstdlib>
#include <format>
#include <future>
#include <print>
#include <string>

#include "hogd/base/cli.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_server.h"
#include "hogd/http/http.h"
#include "hogd/init.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/metrics/shared.h"

namespace {

constexpr uint16_t kDefaultLoadgenPort = 0;

[[noreturn]] void
usage(const char *progname, int exit_code = EXIT_FAILURE)
{
  std::println(stderr,
               "Usage: {} --bind-ip <ip> [--port <port>] "
               "[--metrics-port <port>] [--num-virtual-accts <n>] "
               "[--filler-instances <n>] [--filler-items <n>] "
               "[--filler-item-bytes <n>] [--filler-period <n>] "
               "[--filler-keyspace <n>] [-h|--help] [--version]",
               progname);
  std::println(stderr, "Options:");
  std::println(stderr, "  --bind-ip <ip>  IP address to bind metrics on");
  std::println(stderr,
               "  --port <port>  Port number to listen on (default: {})",
               kDefaultLoadgenPort);
  std::println(stderr, "  --metrics-port <port>  Health/metrics HTTP port "
                       "(default: 0)");
  std::println(stderr,
               "  --filler-instances <n>   Deploy <n> storage_filler "
               "contracts at genesis and send every --filler-period'th work\n"
               "                           transaction to one of them, writing "
               "--filler-items items of\n"
               "                           --filler-item-bytes each. Grows state "
               "with the workload rather\n"
               "                           than with the transaction count; "
               "many instances so the state\n"
               "                           spreads over shards instead of "
               "pinning to one.\n"
               "  --num-virtual-accts <n>  Pay work transfers to one of <n> "
               "synthetic recipients rather than to another account. Nothing "
               "is deployed at them, so the first payment to each inserts a "
               "new db item, and the state grows with the tx count up to <n> "
               "extra items (default: 0, pay other accounts)");
  std::println(stderr, "  --help         Show this help message");
  std::println(stderr, "  -h, --help     Show this help message");
  std::println(stderr, "  --version      Show version information");
  exit(exit_code);
}

uint16_t
parse_port(const std::string &value)
{
  size_t pos = 0;
  uint64_t port = std::stoul(value, &pos, 10);
  if (pos != value.size() || port > 65535) {
    hogd::panic("Invalid port: {}", value);
  }
  return static_cast<uint16_t>(port);
}

uint64_t
parse_count(const std::string &value)
{
  size_t pos = 0;
  uint64_t count = std::stoull(value, &pos, 10);
  if (pos != value.size()) {
    hogd::panic("Invalid count: {}", value);
  }
  return count;
}

} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  if (hogd::help_requested(argc, argv)) {
    usage(argv[0], EXIT_SUCCESS);
  }
  if (hogd::print_version_and_exit_if_requested(argc, argv)) {
    return EXIT_SUCCESS;
  }

  // Initialize hogd
  auto ret = hogd::hogd_init(argc, argv);
  if (ret) {
    return ret;
  }

  const char *progname = argc > 0 ? argv[0] : "loadgen_server";
  std::string bind_ip;
  uint16_t port = kDefaultLoadgenPort;
  uint16_t metrics_port = 0;
  uint64_t num_virtual_accts = 0;
  uint32_t filler_instances = 0;
  uint32_t filler_items = 0;
  uint32_t filler_item_bytes = 0;
  uint32_t filler_period = 1;
  uint64_t filler_keyspace = 0;

  // Parse command line arguments
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--bind-ip" && i + 1 < argc) {
      bind_ip = argv[++i];
      continue;
    }
    if (arg == "--port" && i + 1 < argc) {
      port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--metrics-port" && i + 1 < argc) {
      metrics_port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--num-virtual-accts" && i + 1 < argc) {
      num_virtual_accts = parse_count(argv[++i]);
      continue;
    }
    if (arg == "--filler-instances" && i + 1 < argc) {
      filler_instances = static_cast<uint32_t>(parse_count(argv[++i]));
      continue;
    }
    if (arg == "--filler-items" && i + 1 < argc) {
      filler_items = static_cast<uint32_t>(parse_count(argv[++i]));
      continue;
    }
    if (arg == "--filler-item-bytes" && i + 1 < argc) {
      filler_item_bytes = static_cast<uint32_t>(parse_count(argv[++i]));
      continue;
    }
    if (arg == "--filler-period" && i + 1 < argc) {
      filler_period = static_cast<uint32_t>(parse_count(argv[++i]));
      continue;
    }
    if (arg == "--filler-keyspace" && i + 1 < argc) {
      filler_keyspace = parse_count(argv[++i]);
      continue;
    }
    std::println(stderr, "Error: Unexpected argument '{}'", arg);
    usage(progname);
  }

  if (bind_ip.empty()) {
    usage(progname);
  }

  hogd::info("Starting loadgen server on port {}", port);
  hogd::HttpServer metrics_server;
  hogd::add_health_metrics_routes(metrics_server);
  const uint16_t actual_metrics_port = metrics_server.start_server(
      hogd::make_bind_host_port(bind_ip, metrics_port));
  hogd::info("PORT loadgen.metrics={}", actual_metrics_port);

  // Create and start the loadgen server
  hogd::RpcServer<hogd::LoadgenRpc> server;
  // We set the [max_message_size_] to a large value because the loadgen
  // transfers a lot of data via its RPC calls.
  server->max_message_size_ = hogd::LoadgenRpc::kRpcMaxMessageSize;
  // Must precede start_server: experiments read this as calls arrive.
  server.impl()->num_virtual_addresses_ = num_virtual_accts;
  server.impl()->filler_instances_ = filler_instances;
  server.impl()->filler_items_ = filler_items;
  server.impl()->filler_item_bytes_ = filler_item_bytes;
  server.impl()->filler_period_ = filler_period;
  server.impl()->filler_keyspace_ = filler_keyspace;
  auto actual_port = server.start_server(port);
  hogd::Health::instance().set_status(hogd::Health::Status::kOk)();
  hogd::info("Loadgen server started successfully on port {}", actual_port);

  // block forever
  std::promise<void>().get_future().wait();

  return EXIT_SUCCESS;
}
