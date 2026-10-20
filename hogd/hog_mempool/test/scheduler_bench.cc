#include <benchmark/benchmark.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <ranges>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include "db_sched.hh"
#include "hogd/base/chunked_vector.h"
#include "hogd/base/log.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_mempool/block_transfers.h"
#include "hogd/hog_mempool/scheduler.h"
#include "hogd/hog_mempool/test/infra.h"
#include "hogd/hog_mempool/test/txs.h"
#include "hogd/init.h"
#include "sched_coord.hh"
#include "sched_db.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/x23/async.h"
#include "x23/x23/vec.h"

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
    lookup_reader = std::jthread([&](const std::stop_token &stop) {
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

void
bm_scheduler_expire_transaction_outcomes(benchmark::State &state)
{
  Scheduler scheduler(/*current_block=*/0, nullptr);
  std::vector<TransactionHash> hashes(state.range(0));
  for (size_t i = 0; i < hashes.size(); ++i) {
    std::memcpy(hashes[i].data(), &i, sizeof(i));
  }
  for (auto _ : state) {
    scheduler.record_committed_transaction_outcomes(
        x23::span<const TransactionHash>(hashes), scheduler.current_block())();
    scheduler.set_current_block(scheduler.current_block() +
                                Scheduler::kTransactionOutcomeBlockLifetime -
                                1);
    ElapsedTimer timer;
    scheduler.increment_current_block()();
    state.SetIterationTime(timer.elapsed());
  }
  state.SetItemsProcessed(state.iterations() * hashes.size());
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_scheduler_expire_transaction_outcomes)
    ->Arg(1'000'000)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

void
bm_scheduler_admit_large_batch(benchmark::State &state)
{
  MempoolTestInfra server;
  auto scheduler = server.mempool_coord_rpc().scheduler();
  const auto num_txs = static_cast<size_t>(state.range(0));
  const auto accounts = make_benchmark_accounts(10'000);
  seed_benchmark_hog_ages(server, accounts);
  scheduler->read_average_hog_ages(
      x23::XdrVec<ContractAddress>(accounts.begin(), accounts.end()))();

  fib_client_t fib_client(accounts[0]);
  const SignedTransaction prototype = generate_unsigned_txs(fib_client, 1)[0];
  SignedTransactionBatch txs(num_txs, prototype);
  TransactionHashBatch hashes(num_txs);
  WorkerPool::get_default_pool().for_each_parallel(
      std::views::iota(size_t{0}, num_txs), [&](size_t i) {
        txs[i].tx.invocation.invoked_address = accounts[i % accounts.size()];
        txs[i].tx.uniquifier.nonce = i;
        hashes[i] = sha256(txs[i].tx);
      });
  // Model a warmed-up e2e run: admission probes the outcome cache even for
  // new hashes, while millions of older committed outcomes are retained.
  {
    std::vector<TransactionHash> committed(state.range(1));
    for (size_t i = 0; i < committed.size(); ++i) {
      std::memcpy(committed[i].data(), &i, sizeof(i));
    }
    scheduler->record_committed_transaction_outcomes(
        x23::span<const TransactionHash>(committed), /*height=*/0)();
  }
  for (auto _ : state) {
    ElapsedTimer timer;
    const auto outcomes = scheduler->add_txs(txs, hashes)();
    state.SetIterationTime(timer.elapsed());
    benchmark::DoNotOptimize(outcomes);
    scheduler->remove_from_scheduler(
        x23::span<const TransactionHash>(hashes))();
  }
  state.SetItemsProcessed(state.iterations() * num_txs);
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_scheduler_admit_large_batch)
    ->Args({1'000'000, 0})
    ->Args({1'000'000, 20'000'000})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// Gateway batches are much smaller than a full block. Include a populated
// failure cache: one old failure in a shard makes admission probe that shard
// even when every hash in the incoming batch is new.
void
bm_scheduler_admit_gateway_batch(benchmark::State &state)
{
  MempoolTestInfra server;
  auto scheduler = server.mempool_coord_rpc().scheduler();
  const size_t num_txs = state.range(0);
  const auto accounts = make_benchmark_accounts(state.range(1));
  seed_benchmark_hog_ages(server, accounts);
  scheduler->read_average_hog_ages(
      x23::XdrVec<ContractAddress>(accounts.begin(), accounts.end()))();

  fib_client_t fib_client(accounts[0]);
  const SignedTransaction prototype = generate_unsigned_txs(fib_client, 1)[0];
  SignedTransactionBatch txs(num_txs, prototype);
  TransactionHashBatch hashes(num_txs);
  for (size_t i = 0; i < num_txs; ++i) {
    txs[i].tx.invocation.invoked_address = accounts[i % accounts.size()];
    txs[i].tx.uniquifier.nonce = i;
    hashes[i] = sha256(txs[i].tx);
  }
  for (size_t i = 0; i < static_cast<size_t>(state.range(2)); ++i) {
    TransactionHash hash{};
    std::memcpy(hash.data(), &i, sizeof(i));
    scheduler->record_transaction_outcome(
        hash, SchedulerTransactionStatus::SchedulerTransactionRetry,
        scheduler->current_block())();
  }
  for (auto _ : state) {
    ElapsedTimer timer;
    const auto outcomes = scheduler->add_txs(txs, hashes)();
    state.SetIterationTime(timer.elapsed());
    benchmark::DoNotOptimize(outcomes);
    scheduler->remove_from_scheduler(
        x23::span<const TransactionHash>(hashes))();
  }
  state.SetItemsProcessed(state.iterations() * num_txs);
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_scheduler_admit_gateway_batch)
    ->Args({1024, 1000, 0})
    ->Args({1024, 1000, 10000})
    ->Args({1024, 1, 0})
    ->Args({16384, 1000, 10000})
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

void
bm_mempool_admit_gateway_batch(benchmark::State &state)
{
  MempoolTestInfra server;
  server.mempool_coord_rpc().wait_for_block_commit(1)();
  auto scheduler = server.mempool_coord_rpc().scheduler();
  const size_t batch_size = state.range(0);
  const size_t concurrent_batches = state.range(2);
  const size_t num_txs = batch_size * concurrent_batches;
  const auto accounts = make_benchmark_accounts(state.range(1));
  seed_benchmark_hog_ages(server, accounts);
  for (const auto &account : accounts) {
    server.set_address_auth_data(account, get_default_account());
  }
  scheduler->read_average_hog_ages(
      x23::XdrVec<ContractAddress>(accounts.begin(), accounts.end()))();
  fib_client_t fib_client(accounts[0]);
  const SignedTransaction prototype = generate_unsigned_txs(fib_client, 1)[0];
  SignedTransactionBatch txs(num_txs, prototype);
  TransactionHashBatch hashes(num_txs);
  for (size_t i = 0; i < num_txs; ++i) {
    txs[i].tx.invocation.invoked_address = accounts[i % accounts.size()];
    txs[i].tx.uniquifier.nonce = i;
    hashes[i] = sha256(txs[i].tx);
  }
  std::vector<ParallelSignedTransactionBatch> packed;
  for (size_t i = 0; i < concurrent_batches; ++i) {
    SignedTransactionBatch batch(txs.begin() + (i * batch_size),
                                 txs.begin() + ((i + 1) * batch_size));
    packed.push_back(t_to_parallel_batch(batch)());
  }
  // CI's saturation phase runs after account and auth caches have warmed.
  // Cold per-account DB RPCs must not dominate a short measured iteration.
  for (const auto &batch : packed) {
    const auto outcomes =
        server.mempool_coord_rpc().submit_unvetted_with_outcomes(batch)();
    for (const auto outcome : outcomes) {
      CHECK_EQ(outcome, SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
    }
  }
  scheduler->remove_from_scheduler(x23::span<const TransactionHash>(hashes))();
  auto &metrics = MempoolMetrics::instance();
  const double hold_before = metrics.ingestion_commit_lock_hold_seconds.sum();
  const double wait_before = metrics.ingestion_commit_lock_wait_seconds.sum();
  const std::array<std::pair<const char *, Metrics::Histogram *>, 5> stages{{
      {"account_lookup_us_per_batch", &metrics.scheduler_add_lookup_seconds},
      {"insertion_us_per_batch", &metrics.scheduler_add_insert_seconds},
      {"ready_update_us_per_batch", &metrics.scheduler_add_ready_seconds},
      {"verification_us_per_batch", &metrics.ingestion_verify_seconds},
      {"auth_rpc_us_per_batch", &metrics.ingestion_auth_rpc_seconds},
  }};
  std::array<double, stages.size()> stage_before{};
  for (size_t i = 0; i < stages.size(); ++i) {
    stage_before[i] = stages[i].second->sum();
  }
  for (auto _ : state) {
    auto batches = packed;
    std::vector<x23::Async<SchedGatewaySubmitOutcomeBatch>> handles;
    handles.reserve(batches.size());
    ElapsedTimer timer;
    for (auto &batch : batches) {
      handles.push_back(
          server.mempool_coord_rpc().submit_unvetted_with_outcomes(
              std::move(batch)));
    }
    for (auto &handle : handles) {
      const auto outcomes = handle();
      CHECK_EQ(outcomes.size(), batch_size);
      for (const auto outcome : outcomes) {
        CHECK_EQ(outcome,
                 SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
      }
    }
    state.SetIterationTime(timer.elapsed());
    CHECK_EQ(scheduler->size(), static_cast<ssize_t>(num_txs));
    scheduler->remove_from_scheduler(
        x23::span<const TransactionHash>(hashes))();
  }
  state.SetItemsProcessed(state.iterations() * num_txs);
  const double calls = state.iterations() * concurrent_batches;
  state.counters["admission_lock_hold_us_per_batch"] =
      (metrics.ingestion_commit_lock_hold_seconds.sum() - hold_before) *
      1'000'000 / calls;
  state.counters["admission_lock_wait_us_per_batch"] =
      (metrics.ingestion_commit_lock_wait_seconds.sum() - wait_before) *
      1'000'000 / calls;
  for (size_t i = 0; i < stages.size(); ++i) {
    state.counters[stages[i].first] =
        (stages[i].second->sum() - stage_before[i]) * 1'000'000 / calls;
  }
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_mempool_admit_gateway_batch)
    ->Args({1024, 1000, 1})
    ->Args({1024, 1000, 16})
    // Hold total transactions at 16,384 to measure per-batch coordination.
    ->Args({4096, 1000, 4})
    ->Args({8192, 1000, 2})
    ->Args({16384, 1000, 1})
    ->Args({1024, 1, 1})
    ->Args({1024, 1, 16})
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

// Each callback rewrites the cached auth rows at the next block height while
// retaining their signing rules. Queued transactions carry real Ed25519
// signatures; unnecessary revalidation therefore includes real crypto and RPC
// work. This measures the callback, not full-system transaction throughput.
void
bm_mempool_metadata_auth_update(benchmark::State &state)
{
  MempoolTestInfra server;
  auto &coord = server.mempool_coord_rpc();
  coord.wait_for_block_commit(1)();
  const size_t num_txs = state.range(0);
  const auto accounts = make_benchmark_accounts(state.range(1));
  seed_benchmark_hog_ages(server, accounts);

  AddressAuthData auth_data = get_default_account();
  SecretKey key(CryptographicScheme::Ed25519);
  key.ed25519() = create_key_pair(auth_data, 0, 1);
  auth_data.required_weight = 1;
  auth_data.last_written_height = 2;
  KeyChangeBatch changes;
  for (const auto &account : accounts) {
    server.set_address_auth_data(account, auth_data);
    changes.push_back({account, x23::Ptr<AddressAuthData>(auth_data)});
  }
  BlockHeight height = 2;
  coord.scheduler()->set_current_block(height);
  server.register_auth_data_updates(height, changes)();
  coord.wait_for_block_commit(height)();

  fib_client_t fib_client(accounts[0]);
  const auto prototype = generate_unsigned_txs(fib_client, 1)[0];
  SignedTransactionBatch txs(num_txs, prototype);
  for (size_t i = 0; i < txs.size(); ++i) {
    txs[i].tx.invocation.invoked_address = accounts[i % accounts.size()];
    txs[i].tx.uniquifier.nonce = i;
    txs[i].tx.auth_claim = make_sign_claim(0);
    txs[i].signatures.push_back(
        make_signature(key, txs[i].tx, 0, MempoolTestInfra::kNetworkHash));
  }
  const auto outcomes =
      coord.submit_unvetted_with_outcomes(t_to_parallel_batch(txs)())();
  CHECK_EQ(outcomes.size(), txs.size());
  for (const auto outcome : outcomes) {
    CHECK_EQ(outcome, SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
  }

  auto &metrics = MempoolMetrics::instance();
  const double hold_before = metrics.auth_update_commit_lock_hold_seconds.sum();
  const double verify_before = metrics.auth_update_verify_seconds.sum();
  const auto txs_before = metrics.auth_update_transaction_count.value();
  for (auto _ : state) {
    coord.scheduler()->set_current_block(++height);
    for (auto &change : changes) {
      change.auth_data->last_written_height = height;
    }
    ElapsedTimer timer;
    server.register_auth_data_updates(height, changes)();
    coord.wait_for_block_commit(height)();
    state.SetIterationTime(timer.elapsed());
    CHECK_EQ(coord.scheduler()->size(), static_cast<ssize_t>(num_txs));
  }
  state.counters["admission_lock_hold_us_per_callback"] =
      (metrics.auth_update_commit_lock_hold_seconds.sum() - hold_before) *
      1'000'000 / state.iterations();
  state.counters["auth_rpc_us_per_callback"] =
      (metrics.auth_update_verify_seconds.sum() - verify_before) * 1'000'000 /
      state.iterations();
  state.counters["reauthenticated_transactions_per_callback"] =
      static_cast<double>(metrics.auth_update_transaction_count.value() -
                          txs_before) /
      state.iterations();
  state.SetItemsProcessed(state.iterations());
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_mempool_metadata_auth_update)
    ->Args({1000, 1000})
    ->Args({16000, 1000})
    ->Args({32000, 1000})
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

void
bm_mempool_empty_auth_update(benchmark::State &state)
{
  MempoolTestInfra server;
  auto &coord = server.mempool_coord_rpc();
  coord.wait_for_block_commit(1)();
  BlockHeight height = 1;
  auto &metrics = MempoolMetrics::instance();
  const double hold_before = metrics.auth_update_commit_lock_hold_seconds.sum();
  for (auto _ : state) {
    coord.scheduler()->set_current_block(++height);
    ElapsedTimer timer;
    server.register_auth_data_updates(height, {})();
    coord.wait_for_block_commit(height)();
    state.SetIterationTime(timer.elapsed());
  }
  state.counters["admission_lock_hold_us_per_callback"] =
      (metrics.auth_update_commit_lock_hold_seconds.sum() - hold_before) *
      1'000'000 / state.iterations();
  state.SetItemsProcessed(state.iterations());
}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
BENCHMARK(bm_mempool_empty_auth_update)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

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
