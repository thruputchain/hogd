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
#include "hogd/mempool/registration.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/rpc_from_db.h"
#include "hogd/mempool/rpc_from_event.h"
#include "hogd/mempool/shared.h"
#include "hogd/mempool/test/coordinator.h"
#include "hogd/mempool/test/loadgen.h"
#include "sched_coord.hh"

namespace hogd {

namespace {

constexpr uint16_t kSchedulerServerPort = 7888;

std::string
create_address(uint16_t port)
{
  std::string hostname;
  CHECK_EQ(get_hostname(hostname), 0);
  return std::format("{}:{}", hostname, port);
}

int
coordinator_init(const std::string &mempool_address)
{
  // Start the coordinator server.
  RpcServer<CoordinatorSchedRpc> sched;
  CHECK_EQ(sched.start_server(kSchedulerServerPort), kSchedulerServerPort);

  // Register with the mempool.
  x23::HostPort host_port;
  if (!host_port.parse(mempool_address)) {
    throw std::runtime_error(
        std::format("Invalid mempool address: {}", mempool_address));
  }
  auto client = create_client<SchedulerCoord1>(host_port, /*retry=*/true);
  std::string address_for_sched = create_address(kSchedulerServerPort);
  CoordCard card{.protocol = version::kHogdProtocol,
                 .address_for_sched = address_for_sched.c_str()};
  CHECK_EQ(client.schedulercoord_intro_coord(card)(),
           CoordinatorRegistrationOutcome::CoordinatorRegistrationOK);

  // The coordinator server is initialized. Now block forever.
  info("coordinator initialization complete.");
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
  hogd::info("starting coordinator server!");

  if (argc != 2) {
    std::println("usage: mempool_server_address");
    return EXIT_FAILURE;
  }

  std::string mempool_address = argv[1];
  return hogd::coordinator_init(mempool_address);
}
