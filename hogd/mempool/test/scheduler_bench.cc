#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory>

#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/init.h"
#include "hogd/mempool/scheduler.h"
#include "hogd/mempool/test/infra.h"
#include "hogd/mempool/test/txs.h"
#include "types.hh"

namespace hogd {

namespace {

void
reset_cached_hog_ages(Scheduler &scheduler)
{
  for (const ContractAddress &account : MempoolTestInfra::kAccountAddresses) {
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

void
bm_scheduler_process_transfers(benchmark::State &state)
{
  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  HogsTransferred transfer;
  transfer.block = 5;
  transfer.deltas.reserve(state.range(0));
  for (int i = 0; i < state.range(0); ++i) {
    transfer.deltas.push_back(Delta{
        .address = MempoolTestInfra::kAccountAddresses
            [i % MempoolTestInfra::kAccountAddresses.size()],
        .hog_delta = to_xdr(static_cast<int128_t>(5'000'000'000)),
    });
  }

  scheduler->read_average_hog_ages(transfer)();

  for (auto _ : state) {
    state.PauseTiming();
    reset_cached_hog_ages(*scheduler);
    state.ResumeTiming();
    scheduler->process_transfers(transfer)();
  }
}
BENCHMARK(bm_scheduler_process_transfers)
    ->Arg(1)
    ->Arg(8)
    ->Arg(32)
    ->Arg(128)
    ->Arg(1'000'000)
    ->UseRealTime();

void
bm_scheduler_add_txs(benchmark::State &state)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const size_t num_txs = static_cast<size_t>(state.range(0));

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
BENCHMARK(bm_scheduler_add_txs) // NOLINT
    ->Arg(1)
    ->Arg(8)
    ->Arg(32)
    ->Arg(128)
    ->Arg(1'000'000)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

} // namespace

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret != 0) {
    return EXIT_FAILURE;
  }
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
}
