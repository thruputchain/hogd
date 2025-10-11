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

namespace hogd {

namespace {

constexpr uint16_t kCoordinatorServerPort = 8070;
constexpr uint16_t kExecutorServerPort = 8071;
constexpr uint16_t kDbServerPort = 8072;
constexpr uint16_t kEventServerPort = 8073;

int
mempool_init(size_t num_db_shards, size_t num_exec_shards)
{
  Registration &registration = Registration::registration();

  // Wait for the db shards to connect.
  RpcServer<MempoolDbRpc> db;
  CHECK_EQ(db.start_server(kDbServerPort), kDbServerPort);
  info("Waiting for {} db shards to connect...", num_db_shards);
  while (registration.shards_registered(/*cards=*/nullptr) < num_db_shards) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Wait for the event subsystem to connect.
  RpcServer<MempoolEventRpc> event;
  CHECK_EQ(event.start_server(kEventServerPort), kEventServerPort);
  info("Waiting for event server to connect...");
  while (!registration.event_registered(/*card=*/nullptr)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Let the executors connect.
  RpcServer<MempoolExecutorRpc> exec;
  CHECK_EQ(exec.start_server(kExecutorServerPort), kExecutorServerPort);
  info("Waiting for {} executors to connect...", num_exec_shards);
  while (registration.executors_registered(/*cards=*/nullptr) <
         num_exec_shards) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  RpcServer<MempoolCoordinatorRpc> coord;
  CHECK_EQ(coord.start_server(kCoordinatorServerPort), kCoordinatorServerPort);

  // The mempool is initialized. Now block forever.
  info("mempool initialization complete.");
  std::promise<void>().get_future().wait();

  // TODO(jhumphri): Implement shutdown path.
  // coord.shut_down();
  // exec.shut_down();
  // db.shut_down();

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
  hogd::info("starting mempool!");

  if (argc != 3) {
    std::println("usage: num_db_shards num_executor_shards");
    return EXIT_FAILURE;
  }

  size_t num_db_shards = std::stoul(argv[1], nullptr, 0);
  if (num_db_shards <= 0) {
    std::println("[num_db_shards] must be greater than 0.");
    return EXIT_FAILURE;
  }
  size_t num_exec_shards = std::stoul(argv[1], nullptr, 0);
  if (num_exec_shards <= 0) {
    std::println("[num_exec_shards] must be greater than 0.");
    return EXIT_FAILURE;
  }
  return hogd::mempool_init(num_db_shards, num_exec_shards);
};
