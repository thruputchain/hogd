// hogdb_bench.cc - benchmarks for hogdb

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <memory>
#include <print>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/db.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/invoke_tx.h"
#include "hogd/hogdb/status.h"
#include "hogd/init.h"
#include "hogd/ramdb/db.h"

namespace hogd {

namespace {

// config parameter: location to store ramdb.
std::string db_path;

void
fail(HogDBError err)
{
  panic("Unexpected error: {}", err);
}

// measure returns the execution time of fn in seconds (as a double)
double
measure(auto &&fn)
{
  using clock = std::chrono::high_resolution_clock;
  clock::time_point start = clock::now();
  std::forward<decltype(fn)>(fn)();
  clock::time_point end = clock::now();
  return std::chrono::duration<double>(end - start).count();
}

struct Payment {
  ItemKey src;
  ItemKey dst;
  int128_t amount;
};

class Benchmark {
public:
  static std::unique_ptr<Benchmark>
  make_one_to_many_payments_benchmark(size_t n, int workers);
  static std::unique_ptr<Benchmark>
  make_many_to_one_payments_benchmark(size_t n, int workers);
  static std::unique_ptr<Benchmark>
  make_round_robin_payments_benchmark(size_t n, int workers);

  Benchmark(std::unique_ptr<Database> db, int workers,
            std::vector<Payment> &&ops)
    : db_(std::move(db)), workers_(workers), ops_(std::move(ops))
  {}
  ~Benchmark() = default;

  // run executes the benchmark. This function can be timed directly to measure
  // performance.
  void run();

private:
  HogDatabase db_;
  WorkerPool workers_;
  std::vector<Payment> ops_;
};

constexpr ItemKey
make_key(size_t n)
{
  return make_item_key(make_address(n), n);
}

std::unique_ptr<Database>
open_ramdb(size_t n)
{
  size_t heap_size =
      Database::estimate_heap_size(n * 4, sizeof(ItemKey), sizeof(int128_t));
  int bucket_order = std::countr_zero(std::bit_floor(n));
  HogDBStatus<std::unique_ptr<Database>> db =
      Database::open(db_path, heap_size, bucket_order)
          .transform_error(to_hogdb_error);
  if (!db)
    fail(db.error());
  return std::move(*db);
}

std::unique_ptr<Benchmark>
Benchmark::make_one_to_many_payments_benchmark(size_t n, int workers)
{
  // generate payment ops.
  std::vector<Payment> ops;
  ops.reserve(n - 1);
  for (size_t i = 1; i < n; i++) {
    ops.emplace_back(make_key(0), make_key(i), 1);
  }

  // create the benchmark.
  auto bench =
      std::make_unique<Benchmark>(open_ramdb(n), workers, std::move(ops));

  // set up initial DB state.
  HogDBStatus<void> ret;
  ret = write_nnint128(bench->db_, make_key(0), static_cast<int128_t>(n));
  if (!ret)
    fail(ret.error());
  for (size_t i = 1; i < n; i++) {
    ret = write_nnint128(bench->db_, make_key(i), 0);
    if (!ret)
      fail(ret.error());
  }
  HogDBStatus<uint64_t> lsn = bench->db_.commit();
  if (!lsn)
    fail(lsn.error());

  return bench;
}

std::unique_ptr<Benchmark>
Benchmark::make_many_to_one_payments_benchmark(size_t n, int workers)
{
  // generate payment ops.
  std::vector<Payment> ops;
  ops.reserve(n - 1);
  for (size_t i = 1; i < n; i++) {
    ops.emplace_back(make_key(i), make_key(0), 1);
  }

  // create the benchmark.
  auto bench =
      std::make_unique<Benchmark>(open_ramdb(n), workers, std::move(ops));

  // set up initial DB state.
  HogDBStatus<void> ret;
  ret = write_nnint128(bench->db_, make_key(0), static_cast<int128_t>(0));
  if (!ret)
    fail(ret.error());
  for (size_t i = 1; i < n; i++) {
    ret = write_nnint128(bench->db_, make_key(i), 1);
    if (!ret)
      fail(ret.error());
  }
  HogDBStatus<uint64_t> lsn = bench->db_.commit();
  if (!lsn)
    fail(lsn.error());

  return bench;
}

std::unique_ptr<Benchmark>
Benchmark::make_round_robin_payments_benchmark(size_t n, int workers)
{
  // generate payment ops.
  std::vector<Payment> ops;
  ops.reserve(n);
  for (size_t i = 0; i < n; i++) {
    ops.emplace_back(make_key(i), make_key((i + 1) % n), 1);
  }

  // create the benchmark.
  auto bench =
      std::make_unique<Benchmark>(open_ramdb(n), workers, std::move(ops));

  // set up initial DB state.
  for (size_t i = 0; i < n; i++) {
    if (HogDBStatus<void> ret = write_nnint128(bench->db_, make_key(i), 1);
        !ret)
      fail(ret.error());
  }
  if (HogDBStatus<uint64_t> lsn = bench->db_.commit(); !lsn)
    fail(lsn.error());

  return bench;
}

void
Benchmark::run()
{
  BlockTX block(db_);

  // invoke_tx simulates DB operations from a contract invocation.
  auto invoke_fn = [&block](const Payment &p) {
    InvokeTX tx(block);
    if (HogDBStatus<void> ret = tx.request_nnint128(p.src, -p.amount); !ret)
      fail(ret.error());
    if (HogDBStatus<void> ret = tx.request_nnint128(p.dst, p.amount); !ret)
      fail(ret.error());
    tx.commit();
  };

  // run invoke_tx in parallel on the workers for all payment ops.
  constexpr size_t kBatchesPerWorker = 4;
  size_t width = ops_.size() / workers_.worker_count() / kBatchesPerWorker;
  workers_.for_each_parallel(ops_, invoke_fn, std::max(width, 1UL));

  // commit changes to the ramdb.
  HogDBStatus<void> ret = block.commit().and_then(
      [this] { return db_.commit().transform([](uint64_t lsn) {}); });
  if (!ret)
    fail(ret.error());
}

using bench_fn = std::unique_ptr<Benchmark> (*)(size_t n, int workers);

const std::tuple<std::string, std::string, bench_fn> benchmarks[] = {
    {"roundrobin_payments", "each payer pays exactly one other payer",
     Benchmark::make_round_robin_payments_benchmark},
    {"onetomany_payments", "one payer pays everyone else",
     Benchmark::make_one_to_many_payments_benchmark},
    {"manytoone_payments", "everyone pays one payee",
     Benchmark::make_many_to_one_payments_benchmark},
};

} // namespace

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("starting hogdb_bench!");

  auto print_usage = [] {
    std::println("usage: [db_path] [command] [num] [workers]");
    std::println("commands");
    for (const auto &[name, desc, ign] : hogd::benchmarks)
      std::println("  {}: {}", name, desc);
  };

  if (argc < 4) {
    print_usage();
    return 1;
  }
  hogd::db_path = argv[1];
  std::string cmd = argv[2];
  size_t n = std::stoul(argv[3], nullptr, 0);
  int workers = std::stoi(argv[4], nullptr, 0);

  for (const auto &[name, ign, fn] : hogd::benchmarks) {
    if (cmd != name)
      continue;

    std::unique_ptr<hogd::Benchmark> bench = fn(n, workers);
    double sec = hogd::measure([&bench] { bench->run(); });
    std::println("took {} seconds", sec);
    return 0;
  }

  print_usage();
  return 1;
}
