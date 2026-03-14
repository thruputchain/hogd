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

SignedTransactionBatch
make_schedule_benchmark_txs(size_t num_txs)
{
  SignedTransactionBatch txs;
  txs.reserve(num_txs);
  for (size_t i = 0; i < num_txs; i++) {
    const ContractAddress account =
        make_benchmark_account_address(static_cast<uint8_t>(i + 1));
    fib_client_t fib_client(account);
    SignedTransaction tx = generate_unsigned_txs(fib_client, 1)[0];
    tx.tx.uniquifier.nonce = i;
    txs.push_back(std::move(tx));
  }
  return txs;
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

void
bench_scheduler_process_transfers(benchmark::State &state)
{
  Scheduler scheduler(/*current_block=*/0, /*db_sched_client=*/nullptr);
  HogsTransferred transfer;
  transfer.block = 5;
  transfer.deltas.reserve(state.range(0));

  for (int i = 0; i < state.range(0); i++) {
    const ContractAddress account =
        make_benchmark_account_address(static_cast<uint8_t>(i + 1));
    scheduler.set_cached_hog_age_for_testing(
        HogAge{.address = account,
               .num_hogs = {to_xdr(static_cast<uint128_t>(5'000'000'000 * 10)),
                            to_xdr(static_cast<uint128_t>(0)),
                            to_xdr(static_cast<uint128_t>(0))},
               .average_ages = {0, 0, 0}});
    transfer.deltas.push_back(
        Delta{.address = account,
              .hog_delta = to_xdr(static_cast<int128_t>(5'000'000'000))});
  }

  for (auto _ : state) {
    scheduler.process_transfers(transfer)();
    benchmark::DoNotOptimize(scheduler.tx_cache_size());
  }
}

BENCHMARK(bench_scheduler_process_transfers)
    ->Arg(1)
    ->Arg(8)
    ->Arg(64)
    ->Arg(256)
    ->Iterations(200);

void
bench_scheduler_schedule(benchmark::State &state)
{
  const size_t num_txs = static_cast<size_t>(state.range(0));
  const SignedTransactionBatch txs = make_schedule_benchmark_txs(num_txs);
  std::unordered_set<ContractAddress> accounts;
  accounts.reserve(num_txs);
  for (const SignedTransaction &tx : txs) {
    accounts.insert(tx.tx.invocation.invoked_address);
  }

  for (auto _ : state) {
    state.PauseTiming();
    Scheduler scheduler(/*current_block=*/0, /*db_sched_client=*/nullptr);
    for (const ContractAddress &account : accounts) {
      scheduler.set_cached_hog_age_for_testing(
          HogAge{.address = account,
                 .num_hogs =
                     {to_xdr(static_cast<uint128_t>(5'000'000'000 * 10)),
                      to_xdr(static_cast<uint128_t>(0)),
                      to_xdr(static_cast<uint128_t>(0))},
                 .average_ages = {0, 0, 0}});
    }
    scheduler.add_txs_with_prefetched_accounts(txs, accounts)();
    x23::XdrVec<SignedTransactionHash> scheduled_hashes;
    x23::XdrVec<SignedTransaction> scheduled;
    scheduled_hashes.reserve(num_txs);
    scheduled.reserve(num_txs);
    state.ResumeTiming();

    scheduler.schedule(num_txs, scheduled_hashes, scheduled);
    benchmark::DoNotOptimize(scheduled_hashes.size());
    benchmark::DoNotOptimize(scheduled.size());
  }
}

BENCHMARK(bench_scheduler_schedule)
    ->Arg(1)
    ->Arg(8)
    ->Arg(64)
    ->Arg(256)
    ->Iterations(200);

} // namespace

} // namespace hogd

BENCHMARK_MAIN();
