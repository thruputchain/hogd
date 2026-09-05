#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <utility>

#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/rpc_from_exec.h"
#include "hogd/hog_db/rpc_from_sched.h"
#include "hogd/init.h"
#include "hogd/ramdb/db.h"

namespace hogd {

namespace {

// runs a db server to support an executor benchmark.
void
run_bench_server(uint32_t exec_port, uint32_t coord_port)
{
  // Create a temporary directory for the database
  Directory dir = Directory::open_temporary().value();
  // Generously sized: the execution benches now commit a unique uniquifier per
  // tx (so a single block can hold millions of replay-cache entries for one
  // address at high thread counts), and ramdb fails closed if a snapshot copy
  // runs out of heap. The bench hosts have ample RAM, so reserve plenty.
  constexpr static size_t kDefaultHeapSize = 32ULL * 1024 * 1024 * 1024;

  // Open the database
  auto db_result = Database::open(dir.get_path(), kDefaultHeapSize);
  if (!db_result) {
    panic("Failed to open database");
  }

  HogDbDAO db(RAMDB(std::move(*db_result)));
  RPCContext ctx(std::move(db), nullptr);

  // Start the scheduler server
  std::println("Starting scheduler server on port {}", coord_port);
  HogDbSchedServer db_sched_server(ctx);
  db_sched_server.start_server(coord_port);

  // Start the executor server
  std::println("Starting executor server on port {}", exec_port);
  HogDbExecutorServer db_executor_server(ctx);
  db_executor_server.start_server(exec_port);

  std::fflush(stdout);

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
  std::println("Usage: {} -e <exec_port> -c <sched_port>", argv[0]);
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
  hogd::run_bench_server(*exec_port, *coord_port);
  std::println("bench_server finished with orderly shutdown");
  return 0;
}
