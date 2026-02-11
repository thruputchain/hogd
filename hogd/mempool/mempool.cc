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
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/mempool/registration.h"
#include "hogd/mempool/rpc_from_auth.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/rpc_from_db.h"
#include "hogd/mempool/rpc_from_event.h"
#include "hogd/mempool/rpc_from_exec.h"
#include "port.hh"

namespace hogd {

namespace {

int
mempool_init(size_t num_db_shards, size_t num_exec_shards)
{
  Registration &registration = Registration::registration();

  // Wait for the db shards to connect.
  RpcServer<MempoolDbRpc> db;
  CHECK_EQ(db.start_server(kMempoolDbServerPort),
           uint16_t{kMempoolDbServerPort});
  info("Waiting for {} db shards to connect...", num_db_shards);
  while (registration.shards_registered(/*cards=*/nullptr) < num_db_shards) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Wait for the event subsystem to connect.
  RpcServer<MempoolEventRpc> event;
  CHECK_EQ(event.start_server(kMempoolEventServerPort),
           uint16_t{kMempoolEventServerPort});
  info("Waiting for event server to connect...");
  while (!registration.event_registered(/*card=*/nullptr)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Let the executors connect.
  RpcServer<MempoolExecutorRpc> exec;
  CHECK_EQ(exec.start_server(kMempoolExecutorServerPort),
           uint16_t{kMempoolExecutorServerPort});
  info("Waiting for {} executors to connect...", num_exec_shards);
  while (registration.executors_registered(/*cards=*/nullptr) <
         num_exec_shards) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  RpcServer<MempoolAuthRpc> auth;
  auth->max_message_size_ = LoadgenRpc::kRpcMaxMessageSize;
  CHECK_EQ(auth.start_server(kMempoolAuthServerPort),
           uint16_t{kMempoolAuthServerPort});
  while (!registration.auth_registered(/*card=*/nullptr)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  RpcServer<MempoolCoordinatorRpc> coord(db->impl());
  coord->max_message_size_ = LoadgenRpc::kRpcMaxMessageSize;
  CHECK_EQ(coord.start_server(kMempoolCoordinatorServerPort),
           uint16_t{kMempoolCoordinatorServerPort});
  auth->impl()->register_callback(
      [&coord](BlockHeight height, KeyChangeBatch key_changes) {
        return coord->impl()->auth_data_changed(height, std::move(key_changes));
      });

  while (!coord->impl()->ready()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // The mempool is initialized. Now block forever.
  info("mempool initialization complete.");
  std::promise<void>().get_future().wait();

  // TODO(jhumphri): Implement shutdown path.
  // db.impl()->reset_scheduler()();
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

  size_t num_exec_shards = std::stoul(argv[2], nullptr, 0);
  if (num_exec_shards <= 0) {
    std::println("[num_exec_shards] must be greater than 0.");
    return EXIT_FAILURE;
  }

  return hogd::mempool_init(num_db_shards, num_exec_shards);
}
