#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

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

constexpr GasAmount kBenchmarkTxGas = 5'000'000'000;
constexpr std::array<int64_t, 4> kBenchmarkTxCounts = {1'000, 10'000, 100'000,
                                                       1'000'000};
constexpr std::array<int64_t, 5> kBenchmarkAccountCounts = {1, 16, 1'024,
                                                            32'768, 131'072};
const std::vector<std::vector<int64_t>> kSchedulerBenchmarkArgs = {
    std::vector<int64_t>(kBenchmarkTxCounts.begin(), kBenchmarkTxCounts.end()),
    std::vector<int64_t>(kBenchmarkAccountCounts.begin(),
                         kBenchmarkAccountCounts.end())};

void
reset_cached_hog_ages(Scheduler &scheduler,
                      const std::vector<ContractAddress> &accounts)
{
  for (const ContractAddress &account : accounts) {
    auto [age, lock] = scheduler.get_cached_hog_age(account);
    age->num_hogs[kHogAgeGroupToSchedule] =
        to_xdr(static_cast<uint128_t>(kBenchmarkTxGas * 10));
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
    memcpy(address.data(), &i, sizeof(i));
    accounts.push_back(address);
  }
  return accounts;
}

void
seed_benchmark_hog_ages(MempoolTestInfra &server,
                        const std::vector<ContractAddress> &accounts,
                        size_t num_txs)
{
  if (accounts.empty()) {
    return;
  }

  const size_t base_txs_per_account = num_txs / accounts.size();
  const size_t extra_txs = num_txs % accounts.size();
  for (size_t i = 0; i < accounts.size(); ++i) {
    const size_t txs_for_account = base_txs_per_account + (i < extra_txs);
    const uint128_t num_hogs =
        static_cast<uint128_t>(kBenchmarkTxGas) * txs_for_account;
    server.set_hogs(accounts[i],
                    /*group=*/kHogAgeGroupToSchedule,
                    /*age=*/0,
                    /*num_hogs=*/num_hogs);
  }
}

SignedTransactionBatch
make_benchmark_txs(const std::vector<ContractAddress> &accounts, size_t num_txs)
{
  CHECK(!accounts.empty());

  constexpr char kProgramName[] = "slow_fibonacci";
  constexpr uint32_t kFibonacciN = 10;

  std::vector<fib_client_t> fib_clients;
  fib_clients.reserve(accounts.size());
  for (const ContractAddress &account : accounts) {
    fib_clients.emplace_back(account);
  }

  std::vector<uint64_t> nonces(accounts.size(), 0);
  SignedTransactionBatch txs;
  txs.reserve(num_txs);
  for (size_t i = 0; i < num_txs; ++i) {
    const size_t account_idx = i % accounts.size();
    txs.push_back(SignedTransaction{
        .tx = Transaction{
            .invocation =
                fib_clients[account_idx].make_invocation<kProgramName>(
                    kBenchmarkTxGas, kFibonacciN),
            .uniquifier = {.block_expiry_height =
                               std::numeric_limits<BlockHeight>::max(),
                           .nonce = nonces[account_idx]++},
            .contracts_to_deploy = {},
            .auth_claims = {},
            .gas_claims = {}}});
  }
  return txs;
}

x23::XdrVec<SignedTransactionHash>
make_tx_hashes(const SignedTransactionBatch &txs)
{
  x23::XdrVec<SignedTransactionHash> hashes;
  hashes.reserve(txs.size());
  for (const SignedTransaction &tx : txs) {
    hashes.push_back(sha256(tx.tx));
  }
  return hashes;
}

void
set_throughput_counters(benchmark::State &state, size_t num_txs)
{
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(num_txs));
  state.counters["tx_per_sec"] =
      benchmark::Counter(static_cast<double>(num_txs),
                         benchmark::Counter::kIsIterationInvariantRate);
  state.counters["accounts"] =
      benchmark::Counter(static_cast<double>(state.range(1)),
                         benchmark::Counter::kIsIterationInvariantRate);
}

struct SchedulerBenchmarkFixture {
  std::unique_ptr<MempoolTestInfra> server;
  std::shared_ptr<Scheduler> scheduler;
  SignedTransactionBatch txs;
  x23::XdrVec<SignedTransactionHash> hashes;
  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled_txs;
};

SchedulerBenchmarkFixture
make_scheduler_fixture(size_t num_txs, size_t num_accounts)
{
  SchedulerBenchmarkFixture fixture;
  fixture.server = std::make_unique<MempoolTestInfra>();
  fixture.server->mempool_coord_rpc().wait_for_auth_data(
      /*expected_height=*/0)();
  fixture.scheduler = fixture.server->mempool_coord_rpc().scheduler();
  const std::vector<ContractAddress> accounts =
      make_benchmark_accounts(num_accounts);
  seed_benchmark_hog_ages(*fixture.server, accounts, num_txs);
  fixture.txs = make_benchmark_txs(accounts, num_txs);
  fixture.hashes = make_tx_hashes(fixture.txs);
  return fixture;
}

SchedulerBenchmarkFixture
make_scheduled_fixture(size_t num_txs, size_t num_accounts)
{
  SchedulerBenchmarkFixture fixture =
      make_scheduler_fixture(num_txs, num_accounts);
  fixture.scheduler->add_txs(fixture.txs)();
  fixture.scheduled_hashes.reserve(num_txs);
  fixture.scheduled_txs.reserve(num_txs);
  fixture.scheduler->schedule(num_txs, fixture.scheduled_hashes,
                              fixture.scheduled_txs);
  return fixture;
}

void
bm_scheduler_process_transfers(benchmark::State &state)
{
  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();
  const std::vector<ContractAddress> accounts =
      make_benchmark_accounts(static_cast<size_t>(state.range(0)));
  seed_benchmark_hog_ages(server, accounts, accounts.size() * 10);

  HogsTransferred transfer;
  transfer.block = 5;
  transfer.deltas.reserve(state.range(0));
  for (int i = 0; i < state.range(0); ++i) {
    transfer.deltas.push_back(Delta{
        .address = accounts[static_cast<size_t>(i)],
        .hog_delta = to_xdr(static_cast<int128_t>(kBenchmarkTxGas)),
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
BENCHMARK(bm_scheduler_process_transfers)
    ->Arg(1)
    ->Arg(4)
    ->Arg(16)
    ->Arg(64)
    ->Iterations(1)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

void
bm_scheduler_add_txs(benchmark::State &state)
{
  const size_t num_txs = static_cast<size_t>(state.range(0));
  const size_t num_accounts = static_cast<size_t>(state.range(1));
  const std::vector<ContractAddress> accounts =
      make_benchmark_accounts(num_accounts);

  for (auto _ : state) {
    auto server = std::make_unique<MempoolTestInfra>();
    auto scheduler = server->mempool_coord_rpc().scheduler();
    seed_benchmark_hog_ages(*server, accounts, num_txs);
    const SignedTransactionBatch txs = make_benchmark_txs(accounts, num_txs);

    ElapsedTimer timer;
    scheduler->add_txs(txs)();
    state.SetIterationTime(timer.elapsed());
    benchmark::DoNotOptimize(scheduler->size());
  }

  set_throughput_counters(state, num_txs);
}

void
bm_scheduler_schedule_txs(benchmark::State &state)
{
  const size_t num_txs = static_cast<size_t>(state.range(0));
  const size_t num_accounts = static_cast<size_t>(state.range(1));

  for (auto _ : state) {
    SchedulerBenchmarkFixture fixture =
        make_scheduler_fixture(num_txs, num_accounts);
    fixture.scheduled_hashes.reserve(num_txs);
    fixture.scheduled_txs.reserve(num_txs);
    fixture.scheduler->add_txs(fixture.txs)();

    ElapsedTimer timer;
    fixture.scheduler->schedule(num_txs, fixture.scheduled_hashes,
                                fixture.scheduled_txs);
    state.SetIterationTime(timer.elapsed());

    CHECK_EQ(fixture.scheduled_hashes.size(), num_txs);
    benchmark::DoNotOptimize(fixture.scheduled_hashes);
    benchmark::DoNotOptimize(fixture.scheduled_txs);
  }

  set_throughput_counters(state, num_txs);
}

void
bm_scheduler_remove_validated_txs(benchmark::State &state)
{
  const size_t num_txs = static_cast<size_t>(state.range(0));
  const size_t num_accounts = static_cast<size_t>(state.range(1));

  for (auto _ : state) {
    SchedulerBenchmarkFixture fixture =
        make_scheduler_fixture(num_txs, num_accounts);
    fixture.scheduler->add_txs(fixture.txs)();

    ElapsedTimer timer;
    fixture.scheduler->remove_from_scheduler(
        x23::span<const SignedTransactionHash>(fixture.hashes),
        /*validated_block=*/true)();
    state.SetIterationTime(timer.elapsed());

    CHECK_EQ(fixture.scheduler->size(), 0UL);
    CHECK_EQ(fixture.scheduler->tx_cache_size(), 0UL);
    benchmark::DoNotOptimize(fixture.scheduler->size());
  }

  set_throughput_counters(state, num_txs);
}

void
bm_scheduler_remove_nominated_txs(benchmark::State &state)
{
  const size_t num_txs = static_cast<size_t>(state.range(0));
  const size_t num_accounts = static_cast<size_t>(state.range(1));

  for (auto _ : state) {
    SchedulerBenchmarkFixture fixture =
        make_scheduled_fixture(num_txs, num_accounts);

    ElapsedTimer timer;
    fixture.scheduler->remove_from_scheduler(
        x23::span<const SignedTransactionHash>(fixture.scheduled_hashes),
        /*validated_block=*/false)();
    state.SetIterationTime(timer.elapsed());

    CHECK_EQ(fixture.scheduler->size(), 0UL);
    CHECK_EQ(fixture.scheduler->tx_cache_size(), 0UL);
    benchmark::DoNotOptimize(fixture.scheduler->tx_cache_size());
  }

  set_throughput_counters(state, num_txs);
}

void
bm_scheduler_lifecycle(benchmark::State &state)
{
  const size_t num_txs = static_cast<size_t>(state.range(0));
  const size_t num_accounts = static_cast<size_t>(state.range(1));

  for (auto _ : state) {
    SchedulerBenchmarkFixture fixture =
        make_scheduler_fixture(num_txs, num_accounts);
    fixture.scheduled_hashes.reserve(num_txs);
    fixture.scheduled_txs.reserve(num_txs);

    ElapsedTimer timer;
    fixture.scheduler->add_txs(fixture.txs)();
    fixture.scheduler->schedule(num_txs, fixture.scheduled_hashes,
                                fixture.scheduled_txs);
    fixture.scheduler->remove_from_scheduler(
        x23::span<const SignedTransactionHash>(fixture.scheduled_hashes),
        /*validated_block=*/false)();
    state.SetIterationTime(timer.elapsed());

    CHECK_EQ(fixture.scheduled_hashes.size(), num_txs);
    CHECK_EQ(fixture.scheduler->size(), 0UL);
    CHECK_EQ(fixture.scheduler->tx_cache_size(), 0UL);
    benchmark::DoNotOptimize(fixture.scheduled_hashes);
  }

  set_throughput_counters(state, num_txs);
}

BENCHMARK(bm_scheduler_add_txs) // NOLINT
    ->ArgsProduct(kSchedulerBenchmarkArgs)
    ->Iterations(1)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK(bm_scheduler_schedule_txs) // NOLINT
    ->ArgsProduct(kSchedulerBenchmarkArgs)
    ->Iterations(1)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK(bm_scheduler_remove_validated_txs) // NOLINT
    ->ArgsProduct(kSchedulerBenchmarkArgs)
    ->Iterations(1)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK(bm_scheduler_remove_nominated_txs) // NOLINT
    ->ArgsProduct(kSchedulerBenchmarkArgs)
    ->Iterations(1)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK(bm_scheduler_lifecycle) // NOLINT
    ->ArgsProduct(kSchedulerBenchmarkArgs)
    ->Iterations(1)
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
