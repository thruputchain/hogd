#include <cassert>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "hogd/base/io.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_exec.h"
#include "hogd/hogdb/rpc_from_sched.h"
#include "hogd/init.h"
#include "hogd/ramdb/db.h"
#include "x23/threadpool.h"

namespace hogd {

namespace {

void
run_bench_server(int nthreads, uint32_t exec_port, uint32_t coord_port)
{
  x23::ThreadPool db_pool = x23::ThreadPool(nthreads);

  // Create a temporary directory for the database
  Directory dir = Directory::open_temporary().value();
  constexpr static size_t kDefaultHeapSize = 1024 * 1024 * 1024;

  // Open the database
  auto db_result = Database::open(dir.get_path(), kDefaultHeapSize);
  if (!db_result) {
    throw std::runtime_error("Failed to open database");
  }

  HogDBDAO db(RAMDB(std::move(*db_result)));
  MerkleTriePrefix trie;
  RPCContext ctx(db, trie);

  // Start the coordinator server
  std::println("Starting coordinator server on port {}", coord_port);
  HogDBSchedServer db_sched_server(ctx);
  db_sched_server.start_server(coord_port);

  // Start the executor server
  std::println("Starting executor server on port {}", exec_port);
  HogDBExecutorServer db_executor_server(ctx);
  db_executor_server.start_server(exec_port);

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(100));
  };
}
} // namespace

} // namespace hogd

namespace {
void
usage(char *argv[])
{
  std::println("Usage: {} -e <exec_port> -c <coord_port>", argv[0]);
  exit(EXIT_FAILURE);
}
} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;

  std::optional<int> exec_port;
  std::optional<int> coord_port;

  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "e:c:")) != -1) {
    switch (opt) {
    case 'e':
      exec_port = std::stoi(optarg);
      break;
    case 'c':
      coord_port = std::stoi(optarg);
      break;
    default:
      usage(argv);
    }
  }
  // NOLINTEND(misc-include-cleaner)
  if (!exec_port || !coord_port) {
    usage(argv);
  }
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  hogd::run_bench_server(1, *exec_port, *coord_port);
  std::println("bench_server finished with orderly shutdown");
  return 0;
}
