// Block makespan on heterogeneous workloads.
//
// multiexec_runner measures throughput on uniform payments. This runner asks a
// different question: when a block mixes many light transactions with a few
// heavy ones, how long does the block take, and how much longer do the heavy
// transactions take than they would alone? A heavy transaction that suspends
// (a database read) re-enters the executor's runqueue behind everything that
// is runnable, so its latency in a busy block can stretch well past its
// latency in an empty one, and the block's makespan with it.
//
// Two kinds of heavy transaction, since they stress different things:
//   - fib: fibonacci's slow_fibonacci(n), long-running compute with few
//     suspensions;
//   - reads: storage_reads' read_keys(seed, count), `count` sequential HogDb
//     round trips.
// Light transactions are read_keys with a small count, batched; every heavy
// transaction is dispatched on its own.
//
// Each scenario is one block. For each it prints a `makespan:` line per trial
// and a `makespan_summary:` line across trials. The `_one` scenarios run a
// single heavy transaction in an otherwise empty block; their latency is the
// baseline the heavy latencies in the `_mix` scenarios are compared against.
//
// After the scenarios, `probe:` lines read from hog-db directly, with no
// executor in the path, at increasing numbers of reads in flight: the RPC
// path's own latency and throughput, to set against what the executors see.
//
// The cluster bring-up is multiexec_runner's (see multiexec_cluster.h): the
// runner stands in for the mempool, so none of the mempool's own accounting
// runs.

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <numeric>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "built_contracts/contract_loader.h"
#include "built_contracts/fibonacci.h"
#include "built_contracts/storage_reads.h"
#include "consensus.hh"
#include "contract.hh"
#include "exec_result.hh"
#include "exec_sched.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/network_config_params.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/bench/common_bench_state.h"
#include "hogd/hog_exec/bench/mock_scheduler.h"
#include "hogd/hog_exec/bench/multiexec_cluster.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/item/db_connection_map.h"
#include "hogd/init.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/cpumap.h"
#include "x23/fastclock.h"
#include "x23/rpc.h"
#include "x23/vec.h"

namespace hogd {

namespace {

using fib_meta_t = ContractTraitMeta<fibonacci::Fibonacci>;
using reads_meta_t = ContractTraitMeta<storage_reads::StorageReads>;

// Genesis funding for each bench contract. Every transaction reserves the
// replay-cache storage cost against the contract it invokes, priced from the
// block's snapshot: in a block where the contract's replay cache does not exist
// yet, each one reserves the cost of creating it (~12000). A 100k-transaction
// block therefore needs ~1.2e9, and an underfunded contract fails transactions
// with ExecResultTxNonDeterministicError. 1e15 covers ~8e10 such transactions,
// well inside the native supply.
constexpr int128_t kContractFunding = 1'000'000'000'000'000;

enum class TxClass : uint8_t { kLight, kFib, kReads };

std::string_view
tx_class_name(TxClass c)
{
  switch (c) {
  case TxClass::kLight:
    return "light";
  case TxClass::kFib:
    return "fib";
  case TxClass::kReads:
    return "reads";
  }
  return "?";
}

// Where the heavy transactions sit in the dispatch queue.
enum class HeavyOrder : uint8_t { kFirst, kLast, kShuffled };

std::string_view
heavy_order_name(HeavyOrder o)
{
  switch (o) {
  case HeavyOrder::kFirst:
    return "first";
  case HeavyOrder::kLast:
    return "last";
  case HeavyOrder::kShuffled:
    return "shuffled";
  }
  return "?";
}

struct Params {
  uint32_t fib_n = 31;
  uint32_t heavy_reads = 1000;
  uint32_t light_reads = 2;
  uint32_t n_light = 100'000;
  uint32_t n_heavy = 32;
  uint32_t light_batch = 5;
  uint32_t n_connections_per_executor = 10;
  uint32_t n_coros_per_connection = 100;
  uint32_t n_trials = 5;
};

struct Scenario {
  std::string name;
  uint32_t n_light;
  uint32_t n_heavy;
  TxClass heavy_class;
  HeavyOrder order;
};

// One call to executorsched_propose_batch: txs[start, start + count).
struct WorkUnit {
  TxClass cls;
  size_t start;
  size_t count;
};

struct Block {
  x23::XdrVec<SignedTransaction> txs;
  // In dispatch order.
  std::vector<WorkUnit> units;
};

struct UnitTiming {
  uint64_t start_ns = 0;
  uint64_t end_ns = 0;
};

// Shared by every dispatch coroutine of one block. Each unit index is claimed
// by exactly one coroutine, which alone writes timings[index].
struct Dispatch {
  const Block *block;
  BlockID block_number;
  std::atomic<size_t> next_unit{0};
  std::atomic<size_t> live_coros;
  std::vector<UnitTiming> timings;
  x23::CoroCB<void> *done;
};

x23::Detached
dispatch_coro(exec_client_t &exec_client, Dispatch *d)
{
  co_await default_threadpool().bg();
  for (;;) {
    const size_t i = d->next_unit.fetch_add(1, std::memory_order_relaxed);
    if (i >= d->block->units.size()) {
      break;
    }
    const WorkUnit &unit = d->block->units[i];
    x23::span<const SignedTransaction> txs(d->block->txs.begin() + unit.start,
                                           d->block->txs.begin() + unit.start +
                                               unit.count);
    d->timings[i].start_ns = x23::fastclock::get_ns();
    auto r =
        co_await exec_client.executorsched_propose_batch(d->block_number, txs);
    d->timings[i].end_ns = x23::fastclock::get_ns();
    CHECK_EQ(r.size(), unit.count);
    for (const ExecProposeResult &result : r) {
      if (result.result() != ExecResult::ExecResultOK) {
        panic("{} tx failed: {}", tx_class_name(unit.cls), result.result());
      }
    }
  }
  if (d->live_coros.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    (*d->done)();
  }
}

using db_probe_client_t = DatabaseToExecutorInterfaceV1::client<
    x23::RpcMulticlient<x23::RpcLogger>::ClientRef>;

// Shared by every coroutine of one probe level.
struct Probe {
  BlockID block;
  ContractAddress address;
  std::atomic<int64_t> remaining;
  std::atomic<size_t> live_coros;
  std::atomic<uint64_t> unexpected_errors{0};
  // One per coroutine, which alone appends to it.
  std::vector<std::vector<uint64_t>> latency_ns;
  x23::CoroCB<void> *done;
};

// Reads keys nobody wrote, one at a time, until the probe's budget is spent:
// the cheapest storage read there is, timed from issue to reply.
x23::Detached
probe_coro(db_probe_client_t &client, Probe *p, size_t index)
{
  co_await default_threadpool().bg();
  std::vector<uint64_t> &latency = p->latency_ns[index];
  for (;;) {
    const int64_t n = p->remaining.fetch_sub(1, std::memory_order_relaxed);
    if (n <= 0) {
      break;
    }
    StorageKey key(StorageKeyType::ContractData);
    const std::array<uint64_t, 2> words = {static_cast<uint64_t>(n), index};
    std::memcpy(key.contract_data_key().data(), words.data(),
                std::min(sizeof(words), key.contract_data_key().size()));
    const AddressAndKey addrkey{.address = p->address, .key = std::move(key)};
    const uint64_t start_ns = x23::fastclock::get_ns();
    auto res = co_await client.get_snapshot_item(p->block, addrkey);
    latency.push_back(x23::fastclock::get_ns() - start_ns);
    if (res.error() != RPCError::kRPCNone &&
        res.error() != RPCError::kRPCNotFound) {
      p->unexpected_errors.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (p->live_coros.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    (*p->done)();
  }
}

// Uniquifiers for every work transaction in the run. A nonce is never reused,
// and each expiry height gets at most kBenchUniquifiersPerHeight of them, all
// from one running index, which keeps every invoked address under HogDb's
// per-height replay-cache cap however the blocks are sized. Before each block
// the index skips ahead so that block's expiry heights are not yet expired.
class UniquifierSource {
public:
  UniquifierSource() = default;
  explicit UniquifierSource(BlockHeight first_height)
    : first_height_(static_cast<BlockID>(first_height))
  {}

  // [n_txs] is how many uniquifiers the block will draw.
  void start_block(BlockHeight height, uint64_t n_txs)
  {
    const auto h = static_cast<BlockID>(height);
    CHECK_GE(h, first_height_);
    next_ = std::max(next_, (h - first_height_) *
                                CommonBenchState::kBenchUniquifiersPerHeight);
    // Every expiry height from here to the index's is still unexpired, and
    // HogDb holds at most kReplayCacheHeightsMaxLen of them per address.
    const uint64_t live_heights =
        ((next_ + n_txs) / CommonBenchState::kBenchUniquifiersPerHeight) -
        (h - first_height_) + 1;
    if (live_heights >= kReplayCacheHeightsMaxLen) {
      panic("too many transactions for one run ({} unexpired expiry heights); "
            "lower n_light or trials",
            live_heights);
    }
  }

  Uniquifier next()
  {
    return CommonBenchState::bench_uniquifier(first_height_, next_++);
  }

  // Also unique across the run, so it serves as a per-transaction key seed.
  uint64_t peek() const { return next_; }

private:
  BlockID first_height_ = 0;
  uint64_t next_ = 0;
};

class MakespanBench {
public:
  MakespanBench(const std::vector<std::string> &server_addrs, Params params)
    : params_(params), cluster_(server_addrs)
  {}

  void run()
  {
    exec_clients_ =
        cluster_.connect_exec_clients(params_.n_connections_per_executor);
    run_genesis();

    const std::vector<Scenario> scenarios = {
        {"light_only", params_.n_light, 0, TxClass::kLight, HeavyOrder::kLast},
        {"fib_one", 0, 1, TxClass::kFib, HeavyOrder::kFirst},
        {"fib_mix_first", params_.n_light, params_.n_heavy, TxClass::kFib,
         HeavyOrder::kFirst},
        {"fib_mix_shuffled", params_.n_light, params_.n_heavy, TxClass::kFib,
         HeavyOrder::kShuffled},
        {"fib_mix_last", params_.n_light, params_.n_heavy, TxClass::kFib,
         HeavyOrder::kLast},
        {"reads_one", 0, 1, TxClass::kReads, HeavyOrder::kFirst},
        {"reads_mix_first", params_.n_light, params_.n_heavy, TxClass::kReads,
         HeavyOrder::kFirst},
        {"reads_mix_shuffled", params_.n_light, params_.n_heavy,
         TxClass::kReads, HeavyOrder::kShuffled},
        {"reads_mix_last", params_.n_light, params_.n_heavy, TxClass::kReads,
         HeavyOrder::kLast},
    };

    std::println("makespan_config: nexec={} nconnections={} "
                 "ncoros_per_connection={} fib_n={} heavy_reads={} "
                 "light_reads={} n_light={} n_heavy={} light_batch={} "
                 "trials={}",
                 cluster_.num_executors(), exec_clients_.size(),
                 params_.n_coros_per_connection, params_.fib_n,
                 params_.heavy_reads, params_.light_reads, params_.n_light,
                 params_.n_heavy, params_.light_batch, params_.n_trials);

    uniquifiers_ = UniquifierSource(next_block_height());
    for (const Scenario &s : scenarios) {
      std::vector<uint64_t> makespans;
      std::vector<uint64_t> heavy_maxes;
      for (uint32_t trial = 0; trial < params_.n_trials; trial++) {
        const TrialResult r = run_trial(s, trial);
        makespans.push_back(r.makespan_ns);
        if (s.n_heavy > 0) {
          heavy_maxes.push_back(r.heavy_max_ns);
        }
      }
      std::ranges::sort(makespans);
      std::ranges::sort(heavy_maxes);
      std::println("makespan_summary: scenario={} trials={} "
                   "makespan_min_us={} makespan_median_us={} "
                   "makespan_max_us={} heavy_max_median_us={}",
                   s.name, makespans.size(), makespans.front() / 1000,
                   makespans[makespans.size() / 2] / 1000,
                   makespans.back() / 1000,
                   heavy_maxes.empty()
                       ? 0
                       : heavy_maxes[heavy_maxes.size() / 2] / 1000);
    }

    run_probe();
  }

private:
  Params params_;
  MultiexecCluster cluster_;
  std::vector<exec_client_t> exec_clients_;
  ContractAddress fib_addr_{};
  ContractAddress reads_addr_{};
  // Reset once genesis has committed, at the first work block.
  UniquifierSource uniquifiers_;

  uint64_t parent_block_number_ = 0;
  uint64_t current_block_number_ = 1;
  ConsensusValue prev_value_ = MockSchedEvent::bench_genesis_tip();

  BlockHeight next_block_height() const
  {
    return prev_value_.proposal_header.block_height + 1;
  }

  void run_genesis()
  {
    GenesisConfigurationBuilder builder;

    auto fib_contract = std::make_shared<Contract>(ContractType::kRv32);
    fib_contract->rv32_contract() = load_contract_blob("fibonacci.rv32a");
    builder.add_distribution(fib_contract, SHA256Hash{},
                             fib_meta_t::entrypoint<u8"fibonacci">(),
                             fib_meta_t::make_calldata<u8"fibonacci">(
                                 uint32_t{5}),
                             kContractFunding);
    fib_addr_ = calculate_genesis_deployment_address(sha256(*fib_contract),
                                                     SHA256Hash{});

    auto reads_contract = std::make_shared<Contract>(ContractType::kRv32);
    reads_contract->rv32_contract() = load_contract_blob("storage_reads.rv32a");
    builder.add_distribution(reads_contract, SHA256Hash{},
                             reads_meta_t::entrypoint<u8"init">(),
                             reads_meta_t::make_calldata<u8"init">(),
                             kContractFunding);
    reads_addr_ = calculate_genesis_deployment_address(sha256(*reads_contract),
                                                       SHA256Hash{});

    const StorageCostModel cost_model = StorageCostModel::default_cost_model();
    auto root_contract = load_stock_genesis_root_contract();
    cluster_.write_genesis_configuration(
        builder.build(NetworkHash{}, cost_model, *root_contract,
                      stock_network_config()));

    auto ceremony_txs = builder.make_genesis_ceremony_transactions(cost_model);
    Block ceremony;
    for (auto &tx : ceremony_txs) {
      ceremony.units.push_back(
          {.cls = TxClass::kLight, .start = ceremony.txs.size(), .count = 1});
      ceremony.txs.push_back(std::move(tx));
    }
    run_block(ceremony);
  }

  SignedTransaction make_light_tx()
  {
    ContractTraitClient<storage_reads::StorageReads> client(reads_addr_);
    const uint64_t seed = uniquifiers_.peek();
    return SignedTransaction{
        .tx = Transaction{.invocation = client.make_invocation<u8"read_keys">(
                              kDefaultMaxTxGas, seed, params_.light_reads),
                          .uniquifier = uniquifiers_.next()}};
  }

  SignedTransaction make_heavy_tx(TxClass cls)
  {
    if (cls == TxClass::kFib) {
      ContractTraitClient<fibonacci::Fibonacci> client(fib_addr_);
      return SignedTransaction{
          .tx = Transaction{.invocation =
                                client.make_invocation<u8"slow_fibonacci">(
                                    kDefaultMaxTxGas, params_.fib_n),
                            .uniquifier = uniquifiers_.next()}};
    }
    assert(cls == TxClass::kReads);
    ContractTraitClient<storage_reads::StorageReads> client(reads_addr_);
    const uint64_t seed = uniquifiers_.peek();
    return SignedTransaction{
        .tx = Transaction{.invocation = client.make_invocation<u8"read_keys">(
                              kDefaultMaxTxGas, seed, params_.heavy_reads),
                          .uniquifier = uniquifiers_.next()}};
  }

  Block make_block(const Scenario &s, uint32_t trial)
  {
    uniquifiers_.start_block(next_block_height(),
                             uint64_t{s.n_light} + s.n_heavy);

    Block block;
    std::vector<WorkUnit> light_units;
    for (uint32_t i = 0; i < s.n_light; i += params_.light_batch) {
      const uint32_t count = std::min(params_.light_batch, s.n_light - i);
      light_units.push_back(
          {.cls = TxClass::kLight, .start = block.txs.size(), .count = count});
      for (uint32_t j = 0; j < count; j++) {
        block.txs.push_back(make_light_tx());
      }
    }
    std::vector<WorkUnit> heavy_units;
    for (uint32_t i = 0; i < s.n_heavy; i++) {
      heavy_units.push_back(
          {.cls = s.heavy_class, .start = block.txs.size(), .count = 1});
      block.txs.push_back(make_heavy_tx(s.heavy_class));
    }

    switch (s.order) {
    case HeavyOrder::kFirst:
      block.units = std::move(heavy_units);
      block.units.insert(block.units.end(), light_units.begin(),
                         light_units.end());
      break;
    case HeavyOrder::kLast:
      block.units = std::move(light_units);
      block.units.insert(block.units.end(), heavy_units.begin(),
                         heavy_units.end());
      break;
    case HeavyOrder::kShuffled:
    {
      block.units = std::move(light_units);
      block.units.insert(block.units.end(), heavy_units.begin(),
                         heavy_units.end());
      // Seeded by trial so a rerun dispatches the same order.
      std::mt19937_64 rng(trial);
      std::ranges::shuffle(block.units, rng);
      break;
    }
    }
    return block;
  }

  // Returns the timing of every unit, relative to the start of dispatch.
  std::vector<UnitTiming> run_block(const Block &block)
  {
    open_block(cluster_.db_sched_client(), cluster_.event_sched_client(),
               exec_clients_, parent_block_number_, current_block_number_,
               next_block_height());

    auto [async, cb] = x23::make_async<void>();
    Dispatch d{
        .block = &block,
        .block_number = current_block_number_,
        .live_coros = exec_clients_.size() * params_.n_coros_per_connection,
        .timings = std::vector<UnitTiming>(block.units.size()),
        .done = &cb,
    };
    const uint64_t start_ns = x23::fastclock::get_ns();
    // Round-robin over connections, which alternate executors: each
    // coroutine claims units as soon as it starts, so launching one
    // connection's coroutines at a time would hand the first units of the
    // dispatch order (all the heavy ones, in `_first`) to one executor.
    for (uint32_t i = 0; i < params_.n_coros_per_connection; i++) {
      for (auto &exec_client : exec_clients_) {
        dispatch_coro(exec_client, &d);
      }
    }
    async();

    close_block(cluster_.db_sched_client(), cluster_.event_sched_client(),
                exec_clients_, current_block_number_, prev_value_,
                /*indexer_probe=*/nullptr);
    parent_block_number_ = current_block_number_;
    current_block_number_++;

    for (UnitTiming &t : d.timings) {
      t.start_ns -= start_ns;
      t.end_ns -= start_ns;
    }
    return std::move(d.timings);
  }

  // Reads from hog-db directly, the way executors do but with nothing else
  // in the path, at increasing numbers of reads in flight over as many
  // connections as the executors open between them. Each level prints a
  // `probe:` line. Run inside its own (empty) block, after the scenarios.
  void run_probe()
  {
    const size_t n_connections =
        static_cast<size_t>(db_connection_count()) * cluster_.num_executors();
    // The connection sweep at a fixed, deep in-flight count says whether
    // throughput under load is capped per connection (it scales with the
    // count) or process-wide (it stays flat).
    struct Level {
      size_t inflight;
      size_t connections;
    };
    const std::array<Level, 9> levels = {{
        {.inflight = 1, .connections = 1},
        {.inflight = n_connections, .connections = n_connections},
        {.inflight = 10 * n_connections, .connections = n_connections},
        {.inflight = 30 * n_connections, .connections = n_connections},
        {.inflight = 100 * n_connections, .connections = n_connections},
        {.inflight = 100 * n_connections, .connections = 2 * n_connections},
        {.inflight = 100 * n_connections, .connections = 4 * n_connections},
        {.inflight = 100 * n_connections, .connections = 8 * n_connections},
        {.inflight = 30 * n_connections, .connections = 4 * n_connections},
    }};
    const size_t max_connections = 8 * n_connections;
    std::vector<db_probe_client_t> clients;
    clients.reserve(max_connections);
    for (size_t i = 0; i < max_connections; i++) {
      clients.push_back(make_rpc_multiclient<DatabaseToExecutorInterfaceV1>(
          cluster_.db_exec_addr(), false, *x23::single_bucket_map));
    }

    open_block(cluster_.db_sched_client(), cluster_.event_sched_client(),
               exec_clients_, parent_block_number_, current_block_number_,
               next_block_height());
    for (const auto [inflight, connections] : levels) {
      int64_t n_reads = 1'000'000;
      if (inflight == 1) {
        n_reads = 2000;
      }
      else if (inflight <= n_connections) {
        n_reads = 300'000;
      }
      auto [async, cb] = x23::make_async<void>();
      Probe p{.block = current_block_number_,
              .address = reads_addr_,
              .remaining = n_reads,
              .live_coros = inflight,
              .latency_ns = std::vector<std::vector<uint64_t>>(inflight),
              .done = &cb};
      const uint64_t start_ns = x23::fastclock::get_ns();
      for (size_t i = 0; i < inflight; i++) {
        probe_coro(clients[i % connections], &p, i);
      }
      async();
      const uint64_t end_ns = x23::fastclock::get_ns();

      std::vector<uint64_t> latency;
      latency.reserve(static_cast<size_t>(n_reads));
      for (const auto &v : p.latency_ns) {
        latency.insert(latency.end(), v.begin(), v.end());
      }
      std::ranges::sort(latency);
      const double wall_s = static_cast<double>(end_ns - start_ns) / 1e9;
      const double sum_ns =
          std::accumulate(latency.begin(), latency.end(), 0.0);
      auto pct_us = [&latency](double q) {
        return latency.empty()
                   ? 0.0
                   : static_cast<double>(latency[static_cast<size_t>(
                         q * static_cast<double>(latency.size() - 1))]) /
                         1000.0;
      };
      std::println("probe: inflight={} connections={} reads={} wall_ms={:.1f} "
                   "reads_per_s={:.0f} lat_avg_us={:.1f} lat_p50_us={:.1f} "
                   "lat_p99_us={:.1f} unexpected_errors={}",
                   inflight, connections, latency.size(), wall_s * 1e3,
                   static_cast<double>(latency.size()) / wall_s,
                   latency.empty()
                       ? 0.0
                       : sum_ns / 1000.0 / static_cast<double>(latency.size()),
                   pct_us(0.5), pct_us(0.99), p.unexpected_errors.load());
      std::fflush(stdout);
    }
    close_block(cluster_.db_sched_client(), cluster_.event_sched_client(),
                exec_clients_, current_block_number_, prev_value_,
                /*indexer_probe=*/nullptr);
    parent_block_number_ = current_block_number_;
    current_block_number_++;
  }

  struct TrialResult {
    uint64_t makespan_ns;
    uint64_t heavy_max_ns;
  };

  TrialResult run_trial(const Scenario &s, uint32_t trial)
  {
    const Block block = make_block(s, trial);
    const std::vector<UnitTiming> timings = run_block(block);

    uint64_t makespan = 0;
    uint64_t heavy_done = 0;
    std::vector<uint64_t> light_lat;
    std::vector<uint64_t> heavy_lat;
    for (size_t i = 0; i < block.units.size(); i++) {
      const UnitTiming &t = timings[i];
      makespan = std::max(makespan, t.end_ns);
      if (block.units[i].cls == TxClass::kLight) {
        light_lat.push_back(t.end_ns - t.start_ns);
      }
      else {
        heavy_lat.push_back(t.end_ns - t.start_ns);
        heavy_done = std::max(heavy_done, t.end_ns);
      }
    }
    std::ranges::sort(light_lat);
    std::ranges::sort(heavy_lat);
    auto pct_us = [](const std::vector<uint64_t> &v, double p) -> uint64_t {
      if (v.empty()) {
        return 0;
      }
      return v[static_cast<size_t>(p * static_cast<double>(v.size() - 1))] /
             1000;
    };

    std::println("makespan: scenario={} trial={} nexec={} txs={} units={} "
                 "makespan_us={} light_batch_p50_us={} light_batch_p99_us={} "
                 "heavy={} heavy_order={} heavy_p50_us={} heavy_max_us={} "
                 "last_heavy_done_us={}",
                 s.name, trial, cluster_.num_executors(), block.txs.size(),
                 block.units.size(), makespan / 1000, pct_us(light_lat, 0.5),
                 pct_us(light_lat, 0.99), tx_class_name(s.heavy_class),
                 heavy_order_name(s.order), pct_us(heavy_lat, 0.5),
                 pct_us(heavy_lat, 1.0), heavy_done / 1000);
    std::fflush(stdout);
    return {.makespan_ns = makespan,
            .heavy_max_ns = heavy_lat.empty() ? 0 : heavy_lat.back()};
  }
};

} // namespace

} // namespace hogd

namespace {
[[noreturn]] void
usage(char *argv[])
{
  std::println(
      "Usage: {} -s <server_addr, repeated: event, db, indexer, then executors>"
      " [-f fib_n] [-r heavy_reads] [-R light_reads] [-l n_light]"
      " [-H n_heavy] [-b light_batch] [-c connections_per_executor]"
      " [-k coros_per_connection] [-t trials]",
      argv[0]);
  exit(EXIT_FAILURE);
}

// All of [s] as a uint32_t. Anything else, a value too large to fit
// included, is an error rather than a different workload.
uint32_t
parse_u32(const char *s)
{
  const std::string_view text(s);
  uint32_t value = 0;
  const auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size()) {
    std::println(stderr, "not a 32-bit unsigned integer: '{}'", text);
    exit(EXIT_FAILURE);
  }
  return value;
}
} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;

  std::vector<std::string> server_addrs;
  hogd::Params params;

  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "s:f:r:R:l:H:b:c:k:t:")) != -1) {
    switch (opt) {
    case 's':
      server_addrs.emplace_back(optarg);
      break;
    case 'f':
      params.fib_n = parse_u32(optarg);
      break;
    case 'r':
      params.heavy_reads = parse_u32(optarg);
      break;
    case 'R':
      params.light_reads = parse_u32(optarg);
      break;
    case 'l':
      params.n_light = parse_u32(optarg);
      break;
    case 'H':
      params.n_heavy = parse_u32(optarg);
      break;
    case 'b':
      params.light_batch = parse_u32(optarg);
      break;
    case 'c':
      params.n_connections_per_executor = parse_u32(optarg);
      break;
    case 'k':
      params.n_coros_per_connection = parse_u32(optarg);
      break;
    case 't':
      params.n_trials = parse_u32(optarg);
      break;
    default:
      usage(argv);
    }
  }
  // NOLINTEND(misc-include-cleaner)
  if (server_addrs.size() < 4 || params.light_batch == 0 ||
      params.n_trials == 0 || params.n_connections_per_executor == 0 ||
      params.n_coros_per_connection == 0) {
    usage(argv);
  }

  hogd::MakespanBench(server_addrs, params).run();

  std::fflush(stdout);
  return 0;
}
