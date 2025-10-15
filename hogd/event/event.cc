#include <chrono>
#include <cstdlib>
#include <format>
#include <future>
#include <print>
#include <string>
#include <thread>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/test_util/test_event_aggregator.h"
#include "hogd/init.h"
#include "hogd/mempool/shared.h"
#include "hogd/version.h"
#include "sched_event.hh"

namespace hogd {

namespace {

constexpr uint16_t kExecutorServerPort = 8090;
constexpr uint16_t kSchedulerServerPort = 8091;

std::string
create_address(uint16_t port)
{
  std::string hostname;
  CHECK_EQ(get_hostname(hostname), 0);
  return std::format("{}:{}", hostname, port);
}

int
event_init(const std::string &mempool_address)
{
  // Start the event servers.
  TestEventAggregator event_aggregator;

  RpcServer<TestEventAggregatorExecRpc> exec(event_aggregator);
  CHECK_EQ(exec.start_server(kExecutorServerPort), kExecutorServerPort);

  RpcServer<TestEventAggregatorSchedRpc> sched(event_aggregator);
  CHECK_EQ(sched.start_server(kSchedulerServerPort), kSchedulerServerPort);

  // Register with the mempool.
  x23::HostPort host_port;
  if (!host_port.parse(mempool_address)) {
    throw std::runtime_error(
        std::format("Invalid mempool address: {}", mempool_address));
  }
  auto client = create_client<SchedEvent1>(host_port, /*retry=*/true);
  std::string address_for_exec = create_address(kExecutorServerPort);
  std::string address_for_sched = create_address(kSchedulerServerPort);
  EventCard card{.protocol = version::kHogdProtocol,
                 .upid = {},
                 .address_for_exec = address_for_exec.c_str(),
                 .address_for_sched = address_for_sched.c_str()};
  CHECK_EQ(client.schedevent_intro_event(card)(), RPCError::kRPCNone);

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
  hogd::info("starting event server!");

  if (argc != 2) {
    std::println("usage: mempool_server_address");
    return EXIT_FAILURE;
  }

  std::string mempool_address = argv[1];
  return hogd::event_init(mempool_address);
}
