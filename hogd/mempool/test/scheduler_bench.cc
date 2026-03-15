#include <benchmark/benchmark.h>

#include <cstddef>
#include <memory>

#include "hogd/base/time.h"
#include "hogd/init.h"
#include "hogd/mempool/scheduler.h"
#include "hogd/mempool/test/infra.h"
#include "hogd/mempool/test/txs.h"

namespace hogd {

namespace {

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
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

} // namespace

} // namespace hogd

int
main(int argc, char **argv)
{
  if (hogd_init(argc, argv, /*do_print_version=*/false, LogLevel::kError) !=
      EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
}
