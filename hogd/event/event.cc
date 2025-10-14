#include <chrono>
#include <cstdlib>
#include <format>
#include <future>
#include <print>
#include <string>
#include <thread>

#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/mempool/registration.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/rpc_from_db.h"
#include "hogd/mempool/rpc_from_event.h"
#include "hogd/mempool/rpc_from_exec.h"
#include "hogd/mempool/test/loadgen.h"

namespace hogd {

namespace {

constexpr uint16_t kExecutorServerPort = 8090;
constexpr uint16_t kSchedulerServerPort = 8091;

int
event_init(const std::string &mempool_address)
{

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
