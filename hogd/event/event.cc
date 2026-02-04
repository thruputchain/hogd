#include <cassert>
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
#include "port.hh"
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
           const std::string &work_directory)
{
  // Start the event servers.
  EventRPCContext ctx(work_directory);
  EventExecServer exec_server(ctx);
  EventIndexerServer indexer_server(ctx);
  EventMempoolServer mempool_server(ctx);

  CHECK_EQ(exec_server.start_server(kEventExecutorServerPort),
           uint16_t{kEventExecutorServerPort});
  CHECK_EQ(indexer_server.start_server(kEventIndexerServerPort),
           uint16_t{kEventIndexerServerPort});
  CHECK_EQ(mempool_server.start_server(kEventMempoolServerPort),
           uint16_t{kEventMempoolServerPort});

  // Register with the mempool.
  x23::HostPort host_port;
  if (!host_port.parse(mempool_address)) {
    throw std::runtime_error(
        std::format("Invalid mempool address: {}", mempool_address));
  }
  auto client = connect_to_event_sched_server(mempool_address);
  std::string address_for_exec = create_address(kEventExecutorServerPort);
  std::string address_for_mempool = create_address(kEventMempoolServerPort);
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

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;

  if (argc != 3) {
    std::println("usage: <mempool_server_address> <work_directory>");
    return EXIT_FAILURE;
  }

  hogd::info("starting event server!");

  std::string mempool_address = argv[1];
  std::string work_directory = argv[2];
  return hogd::event_init(mempool_address, work_directory);
}
