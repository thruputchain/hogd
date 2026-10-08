#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "db_sched.hh"
#include "hogd/base/chunked_vector.h"
#include "hogd/base/log.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_mempool/block_transfers.h"
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

  DeltaChunk deltas;
  deltas.reserve(state.range(0));
  for (int i = 0; i < state.range(0); ++i) {
    deltas.push_back(Delta{
        .address = accounts[static_cast<size_t>(i)],
        .hog_delta = to_xdr(static_cast<int128_t>(5'000'000'000)),
    });
  }
  std::vector<DeltaChunk> chunks;
  chunks.push_back(std::move(deltas));
  const BlockTransfers transfer{.block = 5,
                                .deltas = ChunkedVector<DeltaChunk>(
                                    std::move(chunks))};

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
    TransactionHashBatch tx_hashes;
    tx_hashes.reserve(txs.size());
    for (const SignedTransaction &tx : txs) {
      tx_hashes.push_back(sha256(tx.tx));
    }

    ElapsedTimer timer;
    scheduler->add_txs(txs, tx_hashes)();
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
bm_scheduler_record_transaction_outcomes(benchmark::State &state)
{
  Scheduler scheduler(/*current_block=*/0, nullptr);
  std::vector<TransactionHash> hashes(state.range(0));
  for (size_t i = 0; i < hashes.size(); ++i) {
    std::memcpy(hashes[i].data(), &i, sizeof(i));
  }
  BlockHeight height = 0;
  std::jthread lookup_reader;
  if (state.range(2)) {
    lookup_reader = std::jthread([&](std::stop_token stop) {
      while (!stop.stop_requested()) {
        benchmark::DoNotOptimize(
            scheduler.lookup_transaction_outcome(hashes.back())());
      }
    });
  }
  for (auto _ : state) {
    height += Scheduler::kTransactionOutcomeBlockLifetime;
    scheduler.set_current_block(height);
    [&]() -> x23::Async<void> {
      if (state.range(1)) {
        co_await scheduler.record_committed_transaction_outcomes(
            x23::span<const TransactionHash>(hashes), height);
      }
      else {
        for (const TransactionHash &hash : hashes) {
          co_await scheduler.record_transaction_outcome(
              hash, SchedulerTransactionStatus::SchedulerTransactionSucceeded,
              height);
        }
      }
    }()();
  }
  state.SetItemsProcessed(state.iterations() * hashes.size());
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_scheduler_record_transaction_outcomes)
    // Arguments: transaction count, use batching, concurrent outcome lookup.
    ->Args({1'000'000, 0, 0})
    ->Args({1'000'000, 1, 0})
    ->Args({1'000'000, 0, 1})
    ->Args({1'000'000, 1, 1})
    ->UseRealTime()
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
