#include <cstdlib>
#include <format>
#include <print>
#include <stdexcept>
#include <string>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/mempool/shared.h"
#include "hogd/mempool/test/coordinator.h"
#include "hogd/mempool/test/loadgen.h"
#include "hogd/version.h"
#include "sched_coord.hh"
#include "x23/sockio.h"

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
coordinator_init(const std::string &mempool_address,
                 const std::string &loadgen_address)
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

  // We are using loadgen to feed the mempool with traffic. Thus, we need to
  // connect to the loadgen server.
  LoadgenClientManager loadgen(loadgen_address, client,
                               /*num_accounts=*/900);

  info("coordinator initialization complete.");

  // Runs forever.
  loadgen.run(/*print_stats=*/true);

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

  if (argc != 3) {
    std::println("usage: mempool_address loadgen_address");
    return EXIT_FAILURE;
  }

  std::string mempool_address = argv[1];
  std::string loadgen_address = argv[2];
  return hogd::coordinator_init(mempool_address, loadgen_address);
}
