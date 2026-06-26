#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <tuple>
#include <vector>

#include "exec_sched.hh"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/init.h"
#include "hogd/mempool/scheduler.h"
#include "hogd/mempool/test/infra.h"
#include "hogd/mempool/test/txs.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {

namespace {
constexpr BlockID kBenchmarkBlockID = 1234;

template<typename T>
void
bm_xdr_serialize(benchmark::State &state, const T &value, size_t batch_size)
{
  const size_t serialized_size = x23::xdr_size(value);
  for (auto _ : state) {
    x23::Buf serialized = x23::serialize(value);
    benchmark::DoNotOptimize(serialized.data());
    benchmark::ClobberMemory();
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(serialized_size));
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(batch_size));
}

template<typename T>
void
bm_xdr_deserialize(benchmark::State &state, const T &value, size_t batch_size)
{
  const x23::Buf serialized = x23::serialize(value);
  for (auto _ : state) {
    T parsed;
    x23::deserialize(&parsed, x23::BufView(serialized));
    benchmark::DoNotOptimize(parsed);
    benchmark::ClobberMemory();
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(serialized.size()));
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(batch_size));
}

NondeterministicProposalData
make_proposal_data(size_t nondeterministic_bytes, size_t queue_entries)
{
  NondeterministicProposalData data;
  data.nondeterministic_bits.resize(nondeterministic_bytes);
  data.nondeterministic_bit_count =
      static_cast<uint32_t>(nondeterministic_bytes * 8);
  for (size_t i = 0; i < nondeterministic_bytes; ++i) {
    data.nondeterministic_bits[i] = static_cast<uint8_t>(i);
  }

  data.observed_queue_entry_sequence.resize(queue_entries);
  for (size_t i = 0; i < queue_entries; ++i) {
    QueueEntry &entry = data.observed_queue_entry_sequence[i];
    entry.resize(32);
    for (size_t j = 0; j < entry.size(); ++j) {
      entry[j] = static_cast<uint8_t>(i + j);
    }
  }
  return data;
}

ExecProposeBatchResult
make_propose_batch_result(size_t num_txs, size_t nondeterministic_bytes,
                          size_t queue_entries)
{
  const NondeterministicProposalData data =
      make_proposal_data(nondeterministic_bytes, queue_entries);

  ExecProposeBatchResult result;
  result.reserve(num_txs);
  for (size_t i = 0; i < num_txs; ++i) {
    ExecProposeResult tx_result(ExecResult::ExecResultOK);
    tx_result.proposal_data() = data;
    result.push_back(std::move(tx_result));
  }
  return result;
}

SignedTransactionBatch
make_signed_transaction_batch(size_t num_txs)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  return generate_unsigned_txs(fib_client, num_txs);
}

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
    memcpy(address.data(), &i, sizeof(i));
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

void
bm_execsched_propose_batch_args_serialize(benchmark::State &state)
{
  const size_t num_txs = static_cast<size_t>(state.range(0));
  const auto txs = make_signed_transaction_batch(num_txs);
  const ParallelBatch tx_batch = t_to_parallel_batch(txs)();
  const ExecutorSched1::proc_meta<11>::arg_type args{kBenchmarkBlockID,
                                                     tx_batch};
  bm_xdr_serialize(state, args, num_txs);
}
BENCHMARK(bm_execsched_propose_batch_args_serialize)
    ->Arg(1)
    ->Arg(8)
    ->Arg(32)
    ->Arg(128)
    ->Arg(1'000'000)
    ->Unit(benchmark::kMicrosecond);

void
bm_execsched_propose_batch_args_deserialize(benchmark::State &state)
{
  const size_t num_txs = static_cast<size_t>(state.range(0));
  const auto txs = make_signed_transaction_batch(num_txs);
  const ParallelBatch tx_batch = t_to_parallel_batch(txs)();
  const ExecutorSched1::proc_meta<11>::arg_type args{kBenchmarkBlockID,
                                                     tx_batch};
  bm_xdr_deserialize(state, args, num_txs);
}
BENCHMARK(bm_execsched_propose_batch_args_deserialize)
    ->Arg(1)
    ->Arg(8)
    ->Arg(32)
    ->Arg(128)
    ->Arg(1'000'000)
    ->Unit(benchmark::kMicrosecond);

void
bm_execsched_propose_batch_result_serialize(benchmark::State &state)
{
  const size_t num_txs = static_cast<size_t>(state.range(0));
  const size_t nondeterministic_bytes = static_cast<size_t>(state.range(1));
  const size_t queue_entries = static_cast<size_t>(state.range(2));
  const ExecProposeBatchResult result =
      make_propose_batch_result(num_txs, nondeterministic_bytes, queue_entries);
  const ParallelBatch result_batch = t_to_parallel_batch(result)();
  bm_xdr_serialize(state, result_batch, num_txs);
}
BENCHMARK(bm_execsched_propose_batch_result_serialize)
    ->Args({1, 1, 0})
    ->Args({8, 1, 0})
    ->Args({32, 1, 0})
    ->Args({128, 1, 0})
    ->Args({1'000'000, 1, 0})
    ->Args({32, 32, 0})
    ->Args({32, 32, 4})
    ->Unit(benchmark::kMicrosecond);

void
bm_execsched_propose_batch_result_deserialize(benchmark::State &state)
{
  const size_t num_txs = static_cast<size_t>(state.range(0));
  const size_t nondeterministic_bytes = static_cast<size_t>(state.range(1));
  const size_t queue_entries = static_cast<size_t>(state.range(2));
  const ExecProposeBatchResult result =
      make_propose_batch_result(num_txs, nondeterministic_bytes, queue_entries);
  const ParallelBatch result_batch = t_to_parallel_batch(result)();
  bm_xdr_deserialize(state, result_batch, num_txs);
}
BENCHMARK(bm_execsched_propose_batch_result_deserialize)
    ->Args({1, 1, 0})
    ->Args({8, 1, 0})
    ->Args({32, 1, 0})
    ->Args({128, 1, 0})
    ->Args({1'000'000, 1, 0})
    ->Args({32, 32, 0})
    ->Args({32, 32, 4})
    ->Unit(benchmark::kMicrosecond);

void
bm_execsched_validate_batch_args_serialize(benchmark::State &state)
{
  const size_t num_txs = static_cast<size_t>(state.range(0));
  const size_t nondeterministic_bytes = static_cast<size_t>(state.range(1));
  const size_t queue_entries = static_cast<size_t>(state.range(2));
  const auto txs = make_signed_transaction_batch(num_txs);
  const ParallelBatch tx_batch = t_to_parallel_batch(txs)();
  const NondeterministicProposalData data =
      make_proposal_data(nondeterministic_bytes, queue_entries);
  x23::XdrVec<NondeterministicProposalData> tx_nominations;
  tx_nominations.resize(num_txs, data);
  const ParallelBatch tx_nominations_batch =
      t_to_parallel_batch(tx_nominations)();
  const ExecutorSched1::proc_meta<21>::arg_type args{kBenchmarkBlockID,
                                                     tx_batch,
                                                     tx_nominations_batch};
  bm_xdr_serialize(state, args, num_txs);
}
BENCHMARK(bm_execsched_validate_batch_args_serialize)
    ->Args({1, 1, 0})
    ->Args({8, 1, 0})
    ->Args({32, 1, 0})
    ->Args({128, 1, 0})
    ->Args({1'000'000, 1, 0})
    ->Args({32, 32, 0})
    ->Args({32, 32, 4})
    ->Unit(benchmark::kMicrosecond);

void
bm_execsched_validate_batch_args_deserialize(benchmark::State &state)
{
  const size_t num_txs = static_cast<size_t>(state.range(0));
  const size_t nondeterministic_bytes = static_cast<size_t>(state.range(1));
  const size_t queue_entries = static_cast<size_t>(state.range(2));
  const auto txs = make_signed_transaction_batch(num_txs);
  const ParallelBatch tx_batch = t_to_parallel_batch(txs)();
  const NondeterministicProposalData data =
      make_proposal_data(nondeterministic_bytes, queue_entries);
  x23::XdrVec<NondeterministicProposalData> tx_nominations;
  tx_nominations.resize(num_txs, data);
  const ParallelBatch tx_nominations_batch =
      t_to_parallel_batch(tx_nominations)();
  const ExecutorSched1::proc_meta<21>::arg_type args{kBenchmarkBlockID,
                                                     tx_batch,
                                                     tx_nominations_batch};
  bm_xdr_deserialize(state, args, num_txs);
}
BENCHMARK(bm_execsched_validate_batch_args_deserialize)
    ->Args({1, 1, 0})
    ->Args({8, 1, 0})
    ->Args({32, 1, 0})
    ->Args({128, 1, 0})
    ->Args({1'000'000, 1, 0})
    ->Args({32, 32, 0})
    ->Args({32, 32, 4})
    ->Unit(benchmark::kMicrosecond);

void
bm_nomination_proposal_data_parallel_batch_serialize(benchmark::State &state)
{
  const size_t num_txs = static_cast<size_t>(state.range(0));
  const size_t nondeterministic_bytes = static_cast<size_t>(state.range(1));
  const size_t queue_entries = static_cast<size_t>(state.range(2));
  const NondeterministicProposalData data =
      make_proposal_data(nondeterministic_bytes, queue_entries);
  x23::XdrVec<NondeterministicProposalData> tx_nominations;
  tx_nominations.resize(num_txs, data);

  for (auto _ : state) {
    ParallelBatch batch = t_to_parallel_batch(tx_nominations)();
    benchmark::DoNotOptimize(batch.xdr.data());
    benchmark::ClobberMemory();
  }
  const ParallelBatch batch = t_to_parallel_batch(tx_nominations)();
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(batch.xdr.size()));
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(num_txs));
}
BENCHMARK(bm_nomination_proposal_data_parallel_batch_serialize)
    ->Args({1, 1, 0})
    ->Args({8, 1, 0})
    ->Args({32, 1, 0})
    ->Args({128, 1, 0})
    ->Args({1'000'000, 1, 0})
    ->Args({32, 32, 0})
    ->Args({32, 32, 4})
    ->Unit(benchmark::kMicrosecond);

void
bm_nomination_proposal_data_parallel_batch_deserialize(benchmark::State &state)
{
  const size_t num_txs = static_cast<size_t>(state.range(0));
  const size_t nondeterministic_bytes = static_cast<size_t>(state.range(1));
  const size_t queue_entries = static_cast<size_t>(state.range(2));
  const NondeterministicProposalData data =
      make_proposal_data(nondeterministic_bytes, queue_entries);
  x23::XdrVec<NondeterministicProposalData> tx_nominations;
  tx_nominations.resize(num_txs, data);
  const ParallelBatch batch = t_to_parallel_batch(tx_nominations)();

  for (auto _ : state) {
    x23::XdrVec<NondeterministicProposalData> parsed;
    const bool ok = parallel_batch_to_t(batch, parsed)();
    benchmark::DoNotOptimize(ok);
    benchmark::DoNotOptimize(parsed.data());
    benchmark::ClobberMemory();
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(batch.xdr.size()));
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(num_txs));
}
BENCHMARK(bm_nomination_proposal_data_parallel_batch_deserialize)
    ->Args({1, 1, 0})
    ->Args({8, 1, 0})
    ->Args({32, 1, 0})
    ->Args({128, 1, 0})
    ->Args({1'000'000, 1, 0})
    ->Args({32, 32, 0})
    ->Args({32, 32, 4})
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
