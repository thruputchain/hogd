#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <future>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <utility>

#include "auth_rpc.hh"
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
#include "types.hh"

namespace hogd {

namespace {

int
mempool_init(size_t num_db_shards, size_t num_exec_shards, uint16_t db_port,
             uint16_t event_port, uint16_t executor_port, uint16_t auth_port,
             uint16_t coordinator_port)
{
  Registration &registration = Registration::registration();

  // Wait for the db shards to connect.
  RpcServer<MempoolDbRpc> db;
  uint16_t actual_db_port = db.start_server(db_port);
  CHECK(actual_db_port == db_port || db_port == 0);
  info("PORT mempool.db={}", actual_db_port);
  info("Waiting for {} db shards to connect...", num_db_shards);
  while (registration.shards_registered(/*cards=*/nullptr) < num_db_shards) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Wait for the event subsystem to connect.
  RpcServer<MempoolEventRpc> event;
  uint16_t actual_event_port = event.start_server(event_port);
  CHECK(actual_event_port == event_port || event_port == 0);
  info("PORT mempool.event={}", actual_event_port);
  info("Waiting for event server to connect...");
  while (!registration.event_registered(/*card=*/nullptr)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Let the executors connect.
  RpcServer<MempoolExecutorRpc> exec;
  uint16_t actual_executor_port = exec.start_server(executor_port);
  CHECK(actual_executor_port == executor_port || executor_port == 0);
  info("PORT mempool.executor={}", actual_executor_port);
  info("Waiting for {} executors to connect...", num_exec_shards);
  while (registration.executors_registered(/*cards=*/nullptr) <
         num_exec_shards) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  RpcServer<MempoolAuthRpc> auth;
  auth->max_message_size_ = LoadgenRpc::kRpcMaxMessageSize;
  uint16_t actual_auth_port = auth.start_server(auth_port);
  CHECK(actual_auth_port == auth_port || auth_port == 0);
  info("PORT mempool.auth={}", actual_auth_port);
  while (!registration.auth_registered(/*card=*/nullptr)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  RpcServer<MempoolCoordinatorRpc> coord(db->impl());
  coord->max_message_size_ = LoadgenRpc::kRpcMaxMessageSize;
  uint16_t actual_coordinator_port = coord.start_server(coordinator_port);
  CHECK(actual_coordinator_port == coordinator_port || coordinator_port == 0);
  info("PORT mempool.coordinator={}", actual_coordinator_port);
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

namespace {

[[noreturn]] void
usage(char *argv[])
{
  std::println(
      "usage: {} --num-db-shards <n> --num-exec-shards <n> "
      "[--db-port <port>] [--event-port <port>] [--executor-port <port>] "
      "[--auth-port <port>] [--coordinator-port <port>]",
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
  hogd::info("starting mempool!");

  std::optional<size_t> num_db_shards;
  std::optional<size_t> num_exec_shards;
  uint16_t db_port = 0;
  uint16_t event_port = 0;
  uint16_t executor_port = 0;
  uint16_t auth_port = 0;
  uint16_t coordinator_port = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--num-db-shards" && i + 1 < argc) {
      num_db_shards = std::stoul(argv[++i], nullptr, 0);
      continue;
    }
    if (arg == "--num-exec-shards" && i + 1 < argc) {
      num_exec_shards = std::stoul(argv[++i], nullptr, 0);
      continue;
    }
    if (arg == "--db-port" && i + 1 < argc) {
      db_port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--event-port" && i + 1 < argc) {
      event_port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--executor-port" && i + 1 < argc) {
      executor_port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--auth-port" && i + 1 < argc) {
      auth_port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--coordinator-port" && i + 1 < argc) {
      coordinator_port = parse_port(argv[++i]);
      continue;
    }
    usage(argv);
  }

  if (!num_db_shards.has_value() || !num_exec_shards.has_value()) {
    usage(argv);
  }

  if (*num_db_shards <= 0) {
    std::println("[num_db_shards] must be greater than 0.");
    return EXIT_FAILURE;
  }

  if (*num_exec_shards <= 0) {
    std::println("[num_exec_shards] must be greater than 0.");
    return EXIT_FAILURE;
  }

  return hogd::mempool_init(*num_db_shards, *num_exec_shards, db_port,
                            event_port, executor_port, auth_port,
                            coordinator_port);
}
