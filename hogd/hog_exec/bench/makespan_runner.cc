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
// Each trial also scrapes the executors' /metrics before and after the block
// and reports the mean storage-read split: `read_rpc_avg_us`, from issuing the
// read to its reply being delivered (network, HogDb, and the wait for the
// executor's RPC reader to run), and `read_hop_avg_us`, the wait to hop back
// onto the executor's thread pool afterwards. The means are over every read in
// the block, light and heavy alike. The same scrapes, plus hog-db's, give
// how many cores each process kept busy over the trial: `db_cores` for hog-db
// and `exec_cores_max` for the busiest executor, against the `db_ncpu` and
// `exec_ncpu` CPUs each may use (printed in `makespan_config:`). A process
// near its CPU count is the one the block is waiting on.
//
// The cluster bring-up is multiexec_runner's (see multiexec_cluster.h): the
// runner stands in for the mempool, so none of the mempool's own accounting
// runs.

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <limits>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h> // IWYU pragma: keep
#include <unistd.h>
#include <utility>
#include <vector>

#include "built_contracts/contract_loader.h"
#include "built_contracts/fibonacci.h"
#include "built_contracts/storage_reads.h"
#include "consensus.hh"
#include "contract.hh"
#include "exec_result.hh"
#include "exec_sched.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/bench/common_bench_state.h"
#include "hogd/hog_exec/bench/mock_scheduler.h"
#include "hogd/hog_exec/bench/multiexec_cluster.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/init.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/fastclock.h"
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

// Storage-read timing an executor exports (hog_exec/shared.h), summed over
// executors. A delta across a trial splits that trial's reads into the RPC
// round trip to HogDb and the hop back onto the executor's thread pool.
struct ReadStats {
  int64_t count = 0;
  int64_t rpc_ns = 0;
  int64_t hop_ns = 0;

  ReadStats operator-(const ReadStats &o) const
  {
    return {.count = count - o.count,
            .rpc_ns = rpc_ns - o.rpc_ns,
            .hop_ns = hop_ns - o.hop_ns};
  }
  double rpc_avg_us() const
  {
    return count > 0 ? static_cast<double>(rpc_ns) / 1000.0 /
                           static_cast<double>(count)
                     : 0.0;
  }
  double hop_avg_us() const
  {
    return count > 0 ? static_cast<double>(hop_ns) / 1000.0 /
                           static_cast<double>(count)
                     : 0.0;
  }
};

// GET http://<ip>:<port>/metrics. Blocking, with a timeout; the runner calls
// it from its own thread, outside any timed region.
std::optional<std::string>
fetch_metrics(const std::string &ip, uint16_t port)
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return std::nullopt;
  }
  // NOLINTBEGIN(misc-include-cleaner): glibc defines these in internal headers
  timeval timeout{.tv_sec = 5, .tv_usec = 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  // NOLINTEND(misc-include-cleaner)
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1 ||
      ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return std::nullopt;
  }
  const std::string request = std::format(
      "GET /metrics HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n\r\n", ip);
  if (::send(fd, request.data(), request.size(), MSG_NOSIGNAL) !=
      static_cast<ssize_t>(request.size())) {
    ::close(fd);
    return std::nullopt;
  }
  // Read until the body named by Content-Length is in, or the server closes.
  std::string response;
  std::optional<size_t> body_end;
  char buf[16384];
  for (;;) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      break;
    }
    response.append(buf, static_cast<size_t>(n));
    if (!body_end) {
      const size_t header_end = response.find("\r\n\r\n");
      if (header_end == std::string::npos) {
        continue;
      }
      const size_t cl = response.find("Content-Length:");
      if (cl == std::string::npos || cl > header_end) {
        continue; // no length; read to close
      }
      body_end = header_end + 4 +
                 std::stoul(response.substr(cl + 15, header_end - cl - 15));
    }
    if (response.size() >= *body_end) {
      break;
    }
  }
  ::close(fd);
  const size_t header_end = response.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    return std::nullopt;
  }
  return response.substr(header_end + 4);
}

// The value text of a line `<name> <value>` in a Prometheus scrape.
std::optional<std::string>
metric_text(std::string_view text, std::string_view name)
{
  size_t pos = 0;
  while ((pos = text.find(name, pos)) != std::string_view::npos) {
    const bool line_start = pos == 0 || text[pos - 1] == '\n';
    const size_t after = pos + name.size();
    if (line_start && after < text.size() && text[after] == ' ') {
      return std::string(text.substr(after + 1));
    }
    pos = after;
  }
  return std::nullopt;
}

// An integer metric, or 0 when the scrape lacks it.
int64_t
metric_value(std::string_view text, std::string_view name)
{
  const auto value = metric_text(text, name);
  return value ? std::stoll(*value) : 0;
}

// A floating-point metric, or 0 when the scrape lacks it.
double
metric_double(std::string_view text, std::string_view name)
{
  const auto value = metric_text(text, name);
  return value ? std::stod(*value) : 0.0;
}

// The CPU time hog-db and each executor had used at one moment
// (process_cpu_seconds_total, hogd/metrics/shared.h). Two samples, over the
// wall time between them, say how many cores each process kept busy.
struct CpuSample {
  uint64_t wall_ns = 0;
  double db_s = 0;
  std::vector<double> exec_s;
};

struct CpuUse {
  double db_cores = 0;
  double exec_cores_max = 0;
};

CpuUse
cpu_use(const CpuSample &before, const CpuSample &after)
{
  const double wall_s =
      static_cast<double>(after.wall_ns - before.wall_ns) / 1e9;
  if (wall_s <= 0) {
    return {};
  }
  CpuUse use{.db_cores = (after.db_s - before.db_s) / wall_s};
  for (size_t i = 0; i < before.exec_s.size() && i < after.exec_s.size(); i++) {
    use.exec_cores_max =
        std::max(use.exec_cores_max,
                 (after.exec_s[i] - before.exec_s[i]) / wall_s);
  }
  return use;
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
                 "trials={} db_ncpu={} exec_ncpu={}",
                 cluster_.num_executors(), exec_clients_.size(),
                 params_.n_coros_per_connection, params_.fib_n,
                 params_.heavy_reads, params_.light_reads, params_.n_light,
                 params_.n_heavy, params_.light_batch, params_.n_trials,
                 scrape_available_cpus(cluster_.db_ip(), kBenchDbMetricsPort),
                 cluster_.exec_ips().empty()
                     ? 0
                     : scrape_available_cpus(cluster_.exec_ips().front(),
                                             kBenchExecMetricsPort));

    uniquifiers_ = UniquifierSource(next_block_height());
    for (const Scenario &s : scenarios) {
      std::vector<uint64_t> makespans;
      std::vector<uint64_t> heavy_maxes;
      std::vector<double> read_rpc_us;
      std::vector<double> read_hop_us;
      std::vector<double> db_cores;
      std::vector<double> exec_cores;
      for (uint32_t trial = 0; trial < params_.n_trials; trial++) {
        const TrialResult r = run_trial(s, trial);
        makespans.push_back(r.makespan_ns);
        if (s.n_heavy > 0) {
          heavy_maxes.push_back(r.heavy_max_ns);
        }
        read_rpc_us.push_back(r.reads.rpc_avg_us());
        read_hop_us.push_back(r.reads.hop_avg_us());
        db_cores.push_back(r.cpu.db_cores);
        exec_cores.push_back(r.cpu.exec_cores_max);
      }
      std::ranges::sort(makespans);
      std::ranges::sort(heavy_maxes);
      std::ranges::sort(read_rpc_us);
      std::ranges::sort(read_hop_us);
      std::ranges::sort(db_cores);
      std::ranges::sort(exec_cores);
      std::println(
          "makespan_summary: scenario={} trials={} "
          "makespan_min_us={} makespan_median_us={} "
          "makespan_max_us={} heavy_max_median_us={} "
          "read_rpc_avg_median_us={:.1f} "
          "read_hop_avg_median_us={:.1f} db_cores_median={:.1f} "
          "exec_cores_max_median={:.1f}",
          s.name, makespans.size(), makespans.front() / 1000,
          makespans[makespans.size() / 2] / 1000, makespans.back() / 1000,
          heavy_maxes.empty() ? 0 : heavy_maxes[heavy_maxes.size() / 2] / 1000,
          read_rpc_us[read_rpc_us.size() / 2],
          read_hop_us[read_hop_us.size() / 2], db_cores[db_cores.size() / 2],
          exec_cores[exec_cores.size() / 2]);
    }
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
                              std::numeric_limits<uint64_t>::max(), seed,
                              params_.light_reads),
                          .uniquifier = uniquifiers_.next()}};
  }

  SignedTransaction make_heavy_tx(TxClass cls)
  {
    if (cls == TxClass::kFib) {
      ContractTraitClient<fibonacci::Fibonacci> client(fib_addr_);
      return SignedTransaction{
          .tx = Transaction{.invocation =
                                client.make_invocation<u8"slow_fibonacci">(
                                    std::numeric_limits<uint64_t>::max(),
                                    params_.fib_n),
                            .uniquifier = uniquifiers_.next()}};
    }
    assert(cls == TxClass::kReads);
    ContractTraitClient<storage_reads::StorageReads> client(reads_addr_);
    const uint64_t seed = uniquifiers_.peek();
    return SignedTransaction{
        .tx = Transaction{.invocation = client.make_invocation<u8"read_keys">(
                              std::numeric_limits<uint64_t>::max(), seed,
                              params_.heavy_reads),
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
    for (auto &exec_client : exec_clients_) {
      for (uint32_t i = 0; i < params_.n_coros_per_connection; i++) {
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

  // Summed over executors. An executor that cannot be scraped is warned
  // about and left out, so a trial's read stats may be partial.
  ReadStats scrape_read_stats()
  {
    ReadStats total;
    for (const std::string &ip : cluster_.exec_ips()) {
      auto text = fetch_metrics(ip, kBenchExecMetricsPort);
      if (!text) {
        warn("could not scrape executor metrics from {}:{}", ip,
             kBenchExecMetricsPort);
        continue;
      }
      total.count += metric_value(*text, "executor_db_read_count");
      total.rpc_ns += metric_value(*text, "executor_db_read_rpc_ns");
      total.hop_ns += metric_value(*text, "executor_db_read_hop_ns");
    }
    return total;
  }

  // A process that cannot be scraped is warned about and reads as zero, so
  // its cores for the trial come out zero or negative.
  static double scrape_cpu_seconds(const std::string &ip, uint16_t port)
  {
    auto text = fetch_metrics(ip, port);
    if (!text) {
      warn("could not scrape process CPU from {}:{}", ip, port);
      return 0;
    }
    return metric_double(*text, "process_cpu_seconds_total");
  }

  static int64_t scrape_available_cpus(const std::string &ip, uint16_t port)
  {
    auto text = fetch_metrics(ip, port);
    return text ? metric_value(*text, "process_available_cpus") : 0;
  }

  CpuSample scrape_cpu()
  {
    CpuSample sample{.wall_ns = x23::fastclock::get_ns(),
                     .db_s = scrape_cpu_seconds(cluster_.db_ip(),
                                                kBenchDbMetricsPort)};
    for (const std::string &ip : cluster_.exec_ips()) {
      sample.exec_s.push_back(scrape_cpu_seconds(ip, kBenchExecMetricsPort));
    }
    return sample;
  }

  struct TrialResult {
    uint64_t makespan_ns;
    uint64_t heavy_max_ns;
    ReadStats reads;
    CpuUse cpu;
  };

  TrialResult run_trial(const Scenario &s, uint32_t trial)
  {
    const Block block = make_block(s, trial);
    const ReadStats reads_before = scrape_read_stats();
    const CpuSample cpu_before = scrape_cpu();
    const std::vector<UnitTiming> timings = run_block(block);
    const CpuUse cpu = cpu_use(cpu_before, scrape_cpu());
    const ReadStats reads = scrape_read_stats() - reads_before;

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
                 "last_heavy_done_us={} reads={} read_rpc_avg_us={:.1f} "
                 "read_hop_avg_us={:.1f} db_cores={:.1f} exec_cores_max={:.1f}",
                 s.name, trial, cluster_.num_executors(), block.txs.size(),
                 block.units.size(), makespan / 1000, pct_us(light_lat, 0.5),
                 pct_us(light_lat, 0.99), tx_class_name(s.heavy_class),
                 heavy_order_name(s.order), pct_us(heavy_lat, 0.5),
                 pct_us(heavy_lat, 1.0), heavy_done / 1000, reads.count,
                 reads.rpc_avg_us(), reads.hop_avg_us(), cpu.db_cores,
                 cpu.exec_cores_max);
    std::fflush(stdout);
    return {.makespan_ns = makespan,
            .heavy_max_ns = heavy_lat.empty() ? 0 : heavy_lat.back(),
            .reads = reads,
            .cpu = cpu};
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

uint32_t
parse_u32(const char *s)
{
  return static_cast<uint32_t>(std::stoul(s));
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
