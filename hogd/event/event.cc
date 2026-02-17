#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <future>
#include <print>
#include <stdexcept>
#include <string>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
#include "hogd/event/rpc_context.h"
#include "hogd/event/rpc_from_exec.h"
#include "hogd/event/rpc_from_indexer.h"
#include "hogd/event/rpc_from_sched.h"
#include "hogd/init.h"
#include "hogd/version.h"
#include "rpc_types.hh"
#include "sched_event.hh"
#include "x23/sockio.h"

namespace hogd {

namespace {

std::string
create_address(uint16_t port)
{
  std::string hostname;
  CHECK_EQ(get_hostname(hostname), 0);
  return std::format("{}:{}", hostname, port);
}

auto
connect_to_event_sched_server(std::string const &address)
{
  x23::HostPort host_port;
  if (!host_port.parse(address)) {
    throw std::runtime_error("Failed to parse db server address");
  }
  return make_srpc_client<SchedEvent1>(host_port, true);
}

int
event_init(const std::string &mempool_address,
           const std::string &work_directory, uint16_t exec_port,
           uint16_t indexer_port, uint16_t mempool_port)
{
  // Start the event servers.
  EventRPCContext ctx(work_directory);
  EventExecServer exec_server(ctx);
  EventIndexerServer indexer_server(ctx);
  EventMempoolServer mempool_server(ctx);

  uint16_t actual_exec_port = exec_server.start_server(exec_port);
  uint16_t actual_indexer_port = indexer_server.start_server(indexer_port);
  uint16_t actual_mempool_port = mempool_server.start_server(mempool_port);
  CHECK(actual_exec_port == exec_port || exec_port == 0);
  CHECK(actual_indexer_port == indexer_port || indexer_port == 0);
  CHECK(actual_mempool_port == mempool_port || mempool_port == 0);
  info("PORT event.exec={}", actual_exec_port);
  info("PORT event.indexer={}", actual_indexer_port);
  info("PORT event.mempool={}", actual_mempool_port);

  // Register with the mempool.
  x23::HostPort host_port;
  if (!host_port.parse(mempool_address)) {
    throw std::runtime_error(
        std::format("Invalid mempool address: {}", mempool_address));
  }
  auto client = connect_to_event_sched_server(mempool_address);
  std::string address_for_exec = create_address(actual_exec_port);
  std::string address_for_mempool = create_address(actual_mempool_port);
  EventCard card{.protocol = version::kHogdProtocol(),
                 .upid = {},
                 .address_for_exec = address_for_exec.c_str(),
                 .address_for_sched = address_for_mempool.c_str()};
  CHECK_EQ(client.schedevent_intro_event(card), RPCError::kRPCNone);

  // The event server is initialized. Now block forever.
  info("event initialization complete.");
  std::promise<void>().get_future().wait();

  // TODO(jhumphri): Implement shutdown path.

  return 0;
}

} // namespace

} // namespace hogd

namespace {

[[noreturn]] void
usage(char *argv[])
{
  std::println(
      "usage: {} --mempool-addr <host:port> --work-dir <path> "
      "[--executor-port <port>] [--indexer-port <port>] "
      "[--mempool-port <port>]",
      argv[0]);
  exit(EXIT_FAILURE);
}

uint16_t
parse_port(const std::string &value)
{
  size_t pos = 0;
  unsigned long port = std::stoul(value, &pos, 10);
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

  std::string mempool_address;
  std::string work_directory;
  uint16_t exec_port = 0;
  uint16_t indexer_port = 0;
  uint16_t mempool_port = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--mempool-addr" && i + 1 < argc) {
      mempool_address = argv[++i];
      continue;
    }
    if (arg == "--work-dir" && i + 1 < argc) {
      work_directory = argv[++i];
      continue;
    }
    if (arg == "--executor-port" && i + 1 < argc) {
      exec_port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--indexer-port" && i + 1 < argc) {
      indexer_port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--mempool-port" && i + 1 < argc) {
      mempool_port = parse_port(argv[++i]);
      continue;
    }
    usage(argv);
  }

  if (mempool_address.empty() || work_directory.empty()) {
    usage(argv);
  }

  hogd::info("starting event server!");
  return hogd::event_init(mempool_address, work_directory, exec_port,
                          indexer_port, mempool_port);
}
