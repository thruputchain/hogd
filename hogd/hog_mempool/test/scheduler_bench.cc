#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "db_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_mempool/scheduler.h"
#include "hogd/hog_mempool/test/infra.h"
#include "hogd/hog_mempool/test/txs.h"
#include "hogd/init.h"
#include "sched_db.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

namespace {

void
reset_cached_hog_ages(Scheduler &scheduler,
                      const std::vector<ContractAddress> &accounts)
{
  for (const ContractAddress &account : accounts) {
    auto [age, lock] = scheduler.get_cached_hog_age(account);
    age->num_hogs[kHogAgeGroupToSchedule] =
        to_xdr(static_cast<uint128_t>(5'000'000'000 * 10));
    age->average_ages[kHogAgeGroupToSchedule] = 0;
    age->num_hogs[kHogAgeGroupAlreadyScheduled] =
        to_xdr(static_cast<uint128_t>(0));
    age->average_ages[kHogAgeGroupAlreadyScheduled] = 0;
    age->num_hogs[kHogAgeGroupReceived] = to_xdr(static_cast<uint128_t>(0));
    age->average_ages[kHogAgeGroupReceived] = 0;
  }
}

std::vector<ContractAddress>
make_benchmark_accounts(size_t num_accounts)
{
  std::vector<ContractAddress> accounts;
  accounts.reserve(num_accounts);
  for (size_t i = 0; i < num_accounts; ++i) {
    ContractAddress address{};
    static_assert(sizeof(i) <= sizeof(address),
                  "benchmark account index does not fit in ContractAddress");
    std::memcpy(address.data(), &i, sizeof(i));
    accounts.push_back(address);
  }
  return accounts;
}

void
seed_benchmark_hog_ages(MempoolTestInfra &server,
                        const std::vector<ContractAddress> &accounts)
{
  for (const ContractAddress &account : accounts) {
    server.set_hogs(account,
                    /*group=*/kHogAgeGroupToSchedule,
                    /*age=*/0,
                    /*num_hogs=*/5'000'000'000 * 10);
  }
}

void
bm_scheduler_process_transfers(benchmark::State &state)
{
  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();
  const std::vector<ContractAddress> accounts =
      make_benchmark_accounts(static_cast<size_t>(state.range(0)));
  seed_benchmark_hog_ages(server, accounts);

  HogsTransferred transfer;
  transfer.block = 5;
  transfer.deltas.reserve(state.range(0));
  for (int i = 0; i < state.range(0); ++i) {
    transfer.deltas.push_back(Delta{
        .address = accounts[static_cast<size_t>(i)],
        .hog_delta = to_xdr(static_cast<int128_t>(5'000'000'000)),
    });
  }

  scheduler->read_average_hog_ages(transfer)();

  for (auto _ : state) {
    state.PauseTiming();
    reset_cached_hog_ages(*scheduler, accounts);
    state.ResumeTiming();
    scheduler->process_transfers(transfer)();
  }
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_scheduler_process_transfers)
    ->Arg(1)
    ->Arg(4)
    ->Arg(16)
    ->Arg(64)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

void
bm_scheduler_add_txs(benchmark::State &state)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const auto num_txs = static_cast<size_t>(state.range(0));

  for (auto _ : state) {
    MempoolTestInfra server;
    auto scheduler = server.mempool_coord_rpc().scheduler();
    const SignedTransactionBatch txs =
        generate_unsigned_txs(fib_client, num_txs);

    ElapsedTimer timer;
    scheduler->add_txs(txs)();
    state.SetIterationTime(timer.elapsed());
    benchmark::DoNotOptimize(scheduler->size());
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(num_txs));
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_scheduler_add_txs)
    ->Arg(1)
    ->Arg(8)
    ->Arg(32)
    ->Arg(128)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

void
bm_scheduler_batch_lifecycle(benchmark::State &state)
{
  const size_t num_txs = state.range(0);
  const auto accounts = make_benchmark_accounts(state.range(1));
  MempoolTestInfra server;
  auto scheduler = server.mempool_coord_rpc().scheduler();
  seed_benchmark_hog_ages(server, accounts);
  fib_client_t client(accounts.front());
  auto txs = generate_unsigned_txs(client, num_txs);
  std::vector<TransactionHash> hashes;
  hashes.reserve(num_txs);
  for (size_t i = 0; i < num_txs; ++i) {
    txs[i].tx.invocation.invoked_address = accounts[i % accounts.size()];
    hashes.push_back(sha256(txs[i].tx));
  }
  double add_seconds = 0;
  double remove_seconds = 0;
  for (auto _ : state) {
    ElapsedTimer add_timer;
    scheduler->add_txs(txs)();
    const double add = add_timer.elapsed();
    ElapsedTimer remove_timer;
    scheduler->remove_from_scheduler(hashes)();
    const double remove = remove_timer.elapsed();
    state.SetIterationTime(add + remove);
    add_seconds += add;
    remove_seconds += remove;
    CHECK_EQ(scheduler->size(), 0UL);
  }
  state.counters["add_ms"] = add_seconds * 1000 / state.iterations();
  state.counters["remove_ms"] = remove_seconds * 1000 / state.iterations();
  state.SetItemsProcessed(state.iterations() * num_txs);
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_scheduler_batch_lifecycle)
    ->Args({1'000'000, 10'000})
    ->Args({1'000'000, 100'000})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

} // namespace

} // namespace hogd

int
main(int argc, char *argv[])
{
  int ret =
      hogd::hogd_init(argc, argv, /*do_print_version=*/false, LogLevel::kError);
  if (ret != 0) {
    return EXIT_FAILURE;
  }
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
}
