#include <benchmark/benchmark.h>

#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <ranges>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include "db_sched.hh"
#include "built_contracts/itc_erc20.h"
#include "built_contracts/simple_wallet_authclaim.h"
#include "hogd/hog_exec/components/contract_trait.h"
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
reset_cached_native_token_ages(Scheduler &scheduler,
                               const std::vector<ContractAddress> &accounts)
{
  for (const ContractAddress &account : accounts) {
    auto [age, lock] = scheduler.get_cached_native_token_age(account);
    age->num_native_tokens[kNativeTokenAgeGroupToSchedule] =
        to_xdr(static_cast<uint128_t>(5'000'000'000 * 10));
    age->average_ages[kNativeTokenAgeGroupToSchedule] = 0;
    age->num_native_tokens[kNativeTokenAgeGroupAlreadyScheduled] =
        to_xdr(static_cast<uint128_t>(0));
    age->average_ages[kNativeTokenAgeGroupAlreadyScheduled] = 0;
    age->num_native_tokens[kNativeTokenAgeGroupReceived] =
        to_xdr(static_cast<uint128_t>(0));
    age->average_ages[kNativeTokenAgeGroupReceived] = 0;
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
seed_benchmark_native_token_ages(MempoolTestInfra &server,
                                 const std::vector<ContractAddress> &accounts)
{
  for (const ContractAddress &account : accounts) {
    server.set_native_tokens(account,
                             /*group=*/kNativeTokenAgeGroupToSchedule,
                             /*age=*/0,
                             /*num_native_tokens=*/5'000'000'000 * 10);
  }
}

void
bm_scheduler_process_transfers(benchmark::State &state)
{
  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();
  const std::vector<ContractAddress> accounts =
      make_benchmark_accounts(static_cast<size_t>(state.range(0)));
  seed_benchmark_native_token_ages(server, accounts);

  DeltaChunk deltas;
  deltas.reserve(state.range(0));
  for (int i = 0; i < state.range(0); ++i) {
    deltas.push_back(Delta{
        .address = accounts[static_cast<size_t>(i)],
        .native_token_delta = to_xdr(static_cast<int128_t>(5'000'000'000)),
    });
  }
  std::vector<DeltaChunk> chunks;
  chunks.push_back(std::move(deltas));
  const BlockTransfers transfer{.block = 5,
                                .deltas = ChunkedVector<DeltaChunk>(
                                    std::move(chunks))};

  scheduler->read_average_native_token_ages(transfer)();

  for (auto _ : state) {
    state.PauseTiming();
    reset_cached_native_token_ages(*scheduler, accounts);
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
  seed_benchmark_native_token_ages(server, accounts);
  scheduler->read_average_native_token_ages(
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

// Private diagnostic: identical transactions and occupancy, optional probes.
void
bm_scheduler_remove_batch_probe(benchmark::State &state)
{
  MempoolTestInfra server;
  auto scheduler = server.mempool_coord_rpc().scheduler();
  const auto n = static_cast<size_t>(state.range(0));
  const auto backlog = static_cast<size_t>(state.range(1));
  const bool enabled = state.range(2) != 0;
  const bool nominated = state.range(3) != 0;
  const bool payment_shape = state.range(4) != 0;
  const auto accounts = make_benchmark_accounts(10'000);
  seed_benchmark_native_token_ages(server, accounts);
  scheduler->read_average_native_token_ages(
      x23::XdrVec<ContractAddress>(accounts.begin(), accounts.end()))();
  fib_client_t fib_client(accounts[0]);
  const SignedTransaction prototype = generate_unsigned_txs(fib_client, 1)[0];
  auto make_txs = [&](size_t count, size_t offset) {
    SignedTransactionBatch txs(count, prototype);
    TransactionHashBatch hashes(count);
    WorkerPool::get_default_pool().for_each_parallel(
        std::views::iota(size_t{0}, count), [&](size_t i) {
          const size_t sender = (i + offset) % accounts.size();
          txs[i].tx.invocation.invoked_address = accounts[sender];
          txs[i].tx.uniquifier.nonce = i + offset;
          if (payment_shape) {
            // Same allocation/XDR shape as ERC20BasicAuthExperimentState,
            // synthetic signature bytes: this directly tests scheduler
            // removal, not cryptographic validity.
            using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
            using wallet_meta =
                ContractTraitMeta<simple_wallet_authclaim::SimpleWalletAuthClaim>;
            const ContractAddress erc20_address = accounts.back();
            const Calldata internal = x23::serialize(
                erc20_address, erc20_meta::entrypoint<u8"transfer">(),
                GasAmount{1'000'000},
                erc20_meta::make_calldata<u8"transfer">(
                    accounts[(sender+1)%accounts.size()],
                    std::tuple<int64_t,uint64_t>{0,10}));
            txs[i].tx.invocation.entrypoint =
                wallet_meta::entrypoint<u8"do_action">();
            txs[i].tx.invocation.gas_limit = GasAmount{100'000};
            txs[i].tx.invocation.calldata =
                wallet_meta::make_calldata<u8"do_action">(internal);
            txs[i].tx.uniquifier.block_expiry_height = 1'000'000'000;
            txs[i].tx.auth_claim = make_sign_claim(0);
            Signature sig(CryptographicScheme::Ed25519);
            sig.ed25519() = {};
            txs[i].signatures.clear();
            txs[i].signatures.push_back(DecoratedSignature{
                .public_key_id = 0, .signature = sig});
          }
          hashes[i] = sha256(txs[i].tx);
        });
    return std::pair{std::move(txs), std::move(hashes)};
  };
  if (backlog) {
    auto [txs, hashes] = make_txs(backlog, n);
    scheduler->add_txs(txs, hashes)();
  }
  CHECK_EQ(scheduler->size(), backlog);
  CHECK_EQ(scheduler->tx_cache_size(), backlog);
  size_t offset = n+backlog;
  size_t cycle = 0;
  for (auto _ : state) {
    ++cycle;
    auto [txs, hashes] = make_txs(n, offset);
    offset += n;
    scheduler->add_txs(txs, hashes)();
    CHECK_EQ(scheduler->size(), backlog + n);
    CHECK_EQ(scheduler->tx_cache_size(), backlog + n);
    TransactionHashBatch nominated_hashes;
    SignedTransactionHashBatch nominated_signed_hashes;
    SignedTransactionBatch nominated_txs;
    if (nominated) {
      scheduler->schedule(n, nominated_hashes, nominated_signed_hashes, nominated_txs)();
      CHECK_EQ(nominated_hashes.size(), n);
      CHECK_EQ(nominated_signed_hashes.size(), n);
      CHECK_EQ(nominated_txs.size(), n);
      CHECK_EQ(scheduler->size(), backlog);
      CHECK_EQ(scheduler->tx_cache_size(), backlog + n);
    }
    SchedulerRemoveProbe probe;
    ElapsedTimer timer;
    scheduler->remove_from_scheduler(
        x23::span<const TransactionHash>(nominated ? nominated_hashes : hashes), enabled ? &probe : nullptr)();
    const double elapsed = timer.elapsed();
    state.SetIterationTime(elapsed);
    CHECK_EQ(scheduler->size(), backlog);
    CHECK_EQ(scheduler->tx_cache_size(), backlog);
    std::fprintf(stderr, "REMOVE_FIXTURE enabled=%d payment_shape=%d cycle=%zu requested=%zu backlog=%zu actual=%zu elapsed_s=%.9f\n",
                 enabled, payment_shape, cycle, n, backlog, scheduler->tx_cache_size(), elapsed);
    if (enabled) {
      CHECK_EQ(probe.removed_from_runqueue, nominated ? 0 : n);
      CHECK_EQ(probe.expired, 0);
      std::fprintf(stderr, "%s\n", probe.receipt(cycle, cycle, n).c_str());
    }
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(bm_scheduler_remove_batch_probe)
    ->Args({1'000, 0, 1, 1, 1})
    ->Args({1'000'000, 5'000'000, 0, 1, 1})
    ->Args({1'000'000, 5'000'000, 1, 1, 1})
    ->Args({1'000'000, 5'000'000, 1, 1, 0})
    ->Args({1'000'000, 7'000'000, 1, 1, 1})
    ->UseManualTime()->Unit(benchmark::kMillisecond);

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
