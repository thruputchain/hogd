#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <future>
#include <print>
#include <string>
#include <thread>

#include "hogd/base/cli.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/types.h"
#include "hogd/hog_event/rpc_context.h"
#include "hogd/hog_event/rpc_from_exec.h"
#include "hogd/hog_event/rpc_from_indexer.h"
#include "hogd/hog_event/rpc_from_sched.h"
#include "hogd/http/http.h"
#include "hogd/init.h"
#include "hogd/metrics/shared.h"
#include "hogd/version.h"
#include "sched_event.hh"
#include "x23/sockio.h"

namespace hogd {

namespace {

auto
connect_to_event_sched_server(std::string const &address)
{
  x23::HostPort host_port;
  if (!host_port.parse(address)) {
    panic("Invalid mempool address: {}", address);
  }
  return make_srpc_client<SchedEvent1>(host_port, true);
}

int
event_init(const std::string &mempool_address, const std::string &bind_ip,
           const std::string &work_directory, uint16_t exec_port,
           uint16_t indexer_port, uint16_t mempool_port, uint16_t metrics_port)
{
  HttpServer metrics_server;
  add_health_metrics_routes(metrics_server);
  const uint16_t actual_metrics_port =
      metrics_server.start_server(make_bind_host_port(bind_ip, metrics_port));
  info("PORT event.metrics={}", actual_metrics_port);

  // Start the event servers.
  EventRPCContext ctx(work_directory);
  EventExecServer exec_server(ctx);
  EventIndexerServer indexer_server(ctx);
  EventMempoolServer mempool_server(ctx);
  mempool_server.max_message_size_ = kEventSchedulerRpcMaxMessageSize;

  uint16_t actual_exec_port =
      exec_server.start_server(make_bind_host_port(bind_ip, exec_port));
  uint16_t actual_indexer_port =
      indexer_server.start_server(make_bind_host_port(bind_ip, indexer_port));
  uint16_t actual_mempool_port =
      mempool_server.start_server(make_bind_host_port(bind_ip, mempool_port));
  CHECK(actual_exec_port == exec_port || exec_port == 0);
  CHECK(actual_indexer_port == indexer_port || indexer_port == 0);
  CHECK(actual_mempool_port == mempool_port || mempool_port == 0);
  info("PORT event.exec={}", actual_exec_port);
  info("PORT event.indexer={}", actual_indexer_port);
  info("PORT event.mempool={}", actual_mempool_port);

  // Register with the mempool. connect_to_event_sched_server validates the
  // address.
  auto client = connect_to_event_sched_server(mempool_address);
  std::string address_for_exec =
      format_advertised_address(bind_ip, actual_exec_port);
  std::string address_for_mempool =
      format_advertised_address(bind_ip, actual_mempool_port);
  EventCard card{.protocol = version::kHogdProtocol(),
                 .upid = {},
                 .address_for_exec = address_for_exec.c_str(),
                 .address_for_sched = address_for_mempool.c_str()};
  constexpr auto kInitialRegistrationRetryDelay = std::chrono::milliseconds(50);
  constexpr auto kMaxRegistrationRetryDelay = std::chrono::milliseconds(1000);
  auto retry_delay = kInitialRegistrationRetryDelay;
  const auto wait_to_retry = [&] {
    std::this_thread::sleep_for(retry_delay);
    retry_delay = std::min(retry_delay * 2, kMaxRegistrationRetryDelay);
  };
  while (true) {
    auto prepared = client.schedevent_prepare_event(card);
    if (prepared.outcome() == EventPrepareOutcome::EventPrepareNotReady) {
      wait_to_retry();
      continue;
    }
    CHECK_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);
    EventRegistrationSnapshot snapshot = prepared.snapshot();
    auto reconciled = ctx.reconcile_outbox_tip(snapshot.tip.block_id,
                                               snapshot.tip.consensus_value)();
    if (!reconciled) {
      panic("event outbox reconciliation failed");
    }
    Health::instance().set_current_block_height(
        snapshot.tip.consensus_value.proposal_header.block_height)();
    auto activated = client.schedevent_activate_event(snapshot);
    if (activated.outcome() == EventActivationOutcome::EventActivationOK) {
      break;
    }
    CHECK_EQ(activated.outcome(), EventActivationOutcome::EventActivationRetry);
    wait_to_retry();
  }

  // Registration and durable outbox reconciliation are complete.
  Health::instance().set_status(Health::Status::kOk)();
  info("event initialization complete.");
  std::promise<void>().get_future().wait();

  // TODO(jhumphri): Implement shutdown path.

  return 0;
}

} // namespace

} // namespace hogd

namespace {

void
print_usage(char *argv[])
{
  std::println("usage: {} --mempool-addr <host:port> --work-dir <path> "
               "--bind-ip <ip> [--executor-port <port>] "
               "[--indexer-port <port>] [--mempool-port <port>] "
               "[--metrics-port <port>] [-h|--help] [--version]",
               argv[0]);
}

[[noreturn]] void
usage(char *argv[])
{
  print_usage(argv);
  exit(EXIT_FAILURE);
}

} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  if (hogd::help_requested(argc, argv)) {
    print_usage(argv);
    return EXIT_SUCCESS;
  }
  if (hogd::print_version_and_exit_if_requested(argc, argv)) {
    return EXIT_SUCCESS;
  }
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;

  std::string mempool_address;
  std::string bind_ip;
  std::string work_directory;
  uint16_t exec_port = 0;
  uint16_t indexer_port = 0;
  uint16_t mempool_port = 0;
  uint16_t metrics_port = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--metrics-port" && i + 1 < argc) {
      metrics_port =
          hogd::parse_port_arg("--metrics-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--mempool-addr" && i + 1 < argc) {
      mempool_address = argv[++i];
      continue;
    }
    if (arg == "--bind-ip" && i + 1 < argc) {
      bind_ip = argv[++i];
      continue;
    }
    if (arg == "--work-dir" && i + 1 < argc) {
      work_directory = argv[++i];
      continue;
    }
    if (arg == "--executor-port" && i + 1 < argc) {
      exec_port =
          hogd::parse_port_arg("--executor-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--indexer-port" && i + 1 < argc) {
      indexer_port =
          hogd::parse_port_arg("--indexer-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--mempool-port" && i + 1 < argc) {
      mempool_port =
          hogd::parse_port_arg("--mempool-port", argv[++i], argv, usage);
      continue;
    }
    usage(argv);
  }

  if (mempool_address.empty() || bind_ip.empty() || work_directory.empty()) {
    usage(argv);
  }

  hogd::info("starting event server!");
  return hogd::event_init(mempool_address, bind_ip, work_directory, exec_port,
                          indexer_port, mempool_port, metrics_port);
}
