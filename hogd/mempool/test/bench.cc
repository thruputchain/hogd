#include <benchmark/benchmark.h>

#include "hogd/mempool/scheduler.h"
#include "hogd/mempool/test/txs.h"

namespace hogd {

namespace {

ContractAddress
make_benchmark_account_address(uint8_t i)
{
  ContractAddress addr{};
  addr[0] = i;
  return addr;
}

void
bench_scheduler_add_txs(benchmark::State &state)
{
  const ContractAddress account = make_benchmark_account_address(3);
  fib_client_t fib_client(account);
  Scheduler scheduler(/*current_block=*/0, /*db_sched_client=*/nullptr);
  scheduler.set_cached_hog_age_for_testing(
      HogAge{.address = account,
             .num_hogs = {to_xdr(static_cast<uint128_t>(5'000'000'000 * 10)),
                          to_xdr(static_cast<uint128_t>(0)),
                          to_xdr(static_cast<uint128_t>(0))},
             .average_ages = {0, 0, 0}});
  std::unordered_set<ContractAddress> accounts = {account};
  uint64_t nonce_base = 0;

  for (auto _ : state) {
    state.PauseTiming();
    SignedTransactionBatch txs =
        generate_unsigned_txs(fib_client, state.range(0));
    for (SignedTransaction &tx : txs) {
      tx.tx.uniquifier.nonce += nonce_base;
    }
    nonce_base += static_cast<uint64_t>(state.range(0));
    state.ResumeTiming();

    scheduler.add_txs_with_prefetched_accounts(txs, accounts)();
    benchmark::DoNotOptimize(scheduler.size());
  }
}

BENCHMARK(bench_scheduler_add_txs)
    ->Arg(1)
    ->Arg(8)
    ->Arg(64)
    ->Arg(256)
    ->Iterations(200);

} // namespace

} // namespace hogd

BENCHMARK_MAIN();
