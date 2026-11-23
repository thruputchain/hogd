#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/rpc_from_exec.h"
#include "hogd/hog_db/rpc_from_sched.h"
#include "hogd/hog_exec/bench/payment_db_timing.h"
#include "hogd/init.h"
#include "hogd/ramdb/db.h"

namespace hogd {

namespace {

using DbClock = std::chrono::steady_clock;

class PaymentDbHandlerSamples {
  // Bench-only binary artifact. Slots stay visible after control_server
  // terminates bench_server without requiring a signal handler or fflush.
  struct Record {
    uint64_t block, tx, phase, elapsed_ns, outcome;
  };
  static_assert(sizeof(Record) == 5 * sizeof(uint64_t));
  static constexpr size_t kBytes = (kPaymentDbSampleLimit + 1) * sizeof(Record);

public:
  explicit PaymentDbHandlerSamples(uint32_t accounts)
  {
    const char *dump_dir = std::getenv("HOG_DUMP_DIR");
    CHECK(dump_dir && *dump_dir);
    const auto path =
        std::format("{}/payment-db-server-{}.bin", dump_dir, accounts);
    const int fd =
        open(path.c_str(), O_CREAT | O_EXCL | O_CLOEXEC | O_RDWR, 0600);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, kBytes) == 0);
    void *mapping =
        mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    CHECK(mapping != MAP_FAILED);
    records_ = static_cast<Record *>(mapping);
  }

  ~PaymentDbHandlerSamples() { munmap(records_, kBytes); }

  void add(BlockID block, TransactionID tx, uint64_t phase,
           DbClock::time_point start, RPCError outcome)
  {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             DbClock::now() - start)
                             .count();
    const uint64_t index = count_.fetch_add(1, std::memory_order_relaxed);
    if (index < kPaymentDbSampleLimit) {
      std::construct_at(&records_[index],
                        Record{block, tx, phase, static_cast<uint64_t>(elapsed),
                               static_cast<uint64_t>(outcome)});
    }
    else if (index == kPaymentDbSampleLimit) {
      std::construct_at(&records_[index], Record{0, 0, 99, 0, 0});
    }
  }

private:
  Record *records_;
  std::atomic<uint64_t> count_ = 0;
};

class ProfiledPaymentDbRpc : public HogDbExecutorRpc {
public:
  ProfiledPaymentDbRpc(RPCContext &ctx, PaymentDbHandlerSamples &samples)
    : HogDbExecutorRpc(ctx), samples_(samples)
  {}

  TryPushSubtransactionResult
  try_push_new_subtransaction(BlockID block, TransactionID tx,
                              StorageDeltaBatch batch)
  {
    if (tx % kPaymentDbSampleStride != 0) {
      return HogDbExecutorRpc::try_push_new_subtransaction(block, tx,
                                                           std::move(batch));
    }
    const auto start = DbClock::now();
    auto reply =
        HogDbExecutorRpc::try_push_new_subtransaction(block, tx,
                                                      std::move(batch));
    samples_.add(block, tx, 1, start, reply.error());
    return reply;
  }

  RPCError commit_transaction(BlockID block, TransactionID tx,
                              NewContractBatch contracts)
  {
    if (tx % kPaymentDbSampleStride != 0) {
      return HogDbExecutorRpc::commit_transaction(block, tx,
                                                  std::move(contracts));
    }
    const auto start = DbClock::now();
    auto reply =
        HogDbExecutorRpc::commit_transaction(block, tx, std::move(contracts));
    samples_.add(block, tx, 2, start, reply);
    return reply;
  }

private:
  PaymentDbHandlerSamples &samples_;
};

[[noreturn]] void
serve_executor(auto &server, uint32_t port)
{
  server.start_server(port);
  std::fflush(stdout);
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(100));
  }
}

// runs a db server to support an executor benchmark.
void
run_bench_server(uint32_t exec_port, uint32_t coord_port, uint32_t accounts)
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
  if (accounts != 0) {
    PaymentDbHandlerSamples samples(accounts);
    RpcServer<ProfiledPaymentDbRpc> server(ctx, samples);
    server.max_message_size_ = kBulkRpcMaxMessageSize;
    serve_executor(server, exec_port);
  }
  HogDbExecutorServer db_executor_server(ctx);
  serve_executor(db_executor_server, exec_port);
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
  uint32_t diagnostic_accounts = 0;

  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "e:c:D:")) != -1) {
    switch (opt) {
    case 'e':
      exec_port = std::stoi(optarg);
      break;
    case 'c':
      coord_port = std::stoi(optarg);
      break;
    case 'D':
    {
      const std::string_view accounts(optarg);
      if (!hogd::payment_db_timing_enabled("hog11") ||
          diagnostic_accounts != 0 || (accounts != "2" && accounts != "10")) {
        std::println(
            stderr, "DB timing requires the isolated hog11 payment diagnostic");
        return 1;
      }
      diagnostic_accounts = accounts == "2" ? 2 : 10;
      break;
    }
    default:
      usage(argv);
    }
  }
  // NOLINTEND(misc-include-cleaner)
  if (!exec_port || !coord_port) {
    usage(argv);
  }
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  hogd::run_bench_server(*exec_port, *coord_port, diagnostic_accounts);
}
