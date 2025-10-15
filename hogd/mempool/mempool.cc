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

constexpr uint16_t kCoordinatorServerPort = 8070;
constexpr uint16_t kExecutorServerPort = 8071;
constexpr uint16_t kDbServerPort = 8072;
constexpr uint16_t kEventServerPort = 8073;

int
mempool_init(size_t num_db_shards, size_t num_exec_shards,
             const std::string &loadgen_address)
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

  RpcServer<MempoolCoordinatorRpc> coord(db.impl_);
  CHECK_EQ(coord.start_server(kCoordinatorServerPort), kCoordinatorServerPort);
  while (!coord.impl_.ready()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::optional<LoadgenClientManager> loadgen =
      loadgen_address.empty()
          ? std::nullopt
          : // We are using loadgen to feed the mempool with traffic. Thus, we
            // need to connect to the loadgen server.
          std::make_optional<LoadgenClientManager>(loadgen_address, coord.impl_,
                                                   /*num_accounts=*/10000);

  // The mempool is initialized. Now block forever.
  info("mempool initialization complete.");
  std::promise<void>().get_future().wait();

  // TODO(jhumphri): Implement shutdown path.
  // loadgen.reset();
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

  if (argc != 3 && argc != 5) {
    std::println("usage: num_db_shards num_executor_shards [--loadgen-address "
                 "<loadgen_server_address>]");
    return EXIT_FAILURE;
  }

  size_t num_db_shards = std::stoul(argv[1], nullptr, 0);
  if (num_db_shards <= 0) {
    std::println("[num_db_shards] must be greater than 0.");
    return EXIT_FAILURE;
  }

  size_t num_exec_shards = std::stoul(argv[2], nullptr, 0);
  if (num_exec_shards <= 0) {
    std::println("[num_exec_shards] must be greater than 0.");
    return EXIT_FAILURE;
  }

  std::string loadgen_address;
  if (argc == 5) {
    std::string flag = argv[3];
    if (flag == "--loadgen-address") {
      loadgen_address = argv[4];
      std::println("loadgen address provided: {}", loadgen_address);
    }
    else {
      std::println("Unexpected flag '{}'. Did you mean '--loadgen-address "
                   "<loadgen_server_address>'?",
                   flag);
      return EXIT_FAILURE;
    }
  }
  return hogd::mempool_init(num_db_shards, num_exec_shards, loadgen_address);
}
