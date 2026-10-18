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
// the block, light and heavy alike. The same scrapes, plus hog-db's, report
// the load over the trial (see LoadStats): cores busy in each process and on
// each host, the busiest core's busy and softirq fractions (from the hosts'
// /proc/stat), and hog-db's per-RPC handler wall and CPU time. Handlers run
// inline on the connection's RPC reader, so handler wall time per second
// (`db_handler_cores`) is how many connection readers were occupied.
//
// After the scenarios, `probe:` lines read from hog-db directly, with no
// executor in the path, at increasing numbers of reads in flight: the RPC
// path's own latency and throughput, to set against what the executors see.
//
// The cluster bring-up is multiexec_runner's (see multiexec_cluster.h): the
// runner stands in for the mempool, so none of the mempool's own accounting
// runs.

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <numeric>
#include <optional>
#include <print>
#include <random>
#include <sstream>
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
#include "executor_to_db_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_db/db_metrics.h"
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

// GET http://<ip>:<port><path>. Blocking, with a timeout; the runner calls
// it from its own thread, outside any timed region.
std::optional<std::string>
fetch_metrics(const std::string &ip, uint16_t port,
              std::string_view path = "/metrics")
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
  const std::string request =
      std::format("GET {} HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n\r\n",
                  path, ip);
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

// Per-core tick counters from one host's /proc/stat, in USER_HZ ticks
// (10 ms). `busy` is everything but idle and iowait; `softirq` is irq plus
// softirq, the kernel's interrupt and network receive processing, which a
// process's own CPU time does not count.
struct CoreTicks {
  int64_t busy = 0;
  int64_t softirq = 0;
  int64_t total = 0;
};

std::vector<CoreTicks>
parse_procstat(std::string_view text)
{
  std::vector<CoreTicks> cores;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string_view::npos) {
      eol = text.size();
    }
    const std::string_view line = text.substr(pos, eol - pos);
    pos = eol + 1;
    // Only the per-core "cpuN" lines; the aggregate "cpu " line has no digit.
    if (line.size() < 4 || !line.starts_with("cpu") ||
        std::isdigit(static_cast<unsigned char>(line[3])) == 0) {
      continue;
    }
    // user nice system idle iowait irq softirq steal
    std::array<int64_t, 8> f{};
    std::istringstream in{std::string(line.substr(line.find(' ')))};
    for (int64_t &v : f) {
      in >> v;
    }
    const int64_t softirq = f[5] + f[6];
    const int64_t busy = f[0] + f[1] + f[2] + softirq + f[7];
    cores.push_back(
        {.busy = busy, .softirq = softirq, .total = busy + f[3] + f[4]});
  }
  return cores;
}

// One host across a trial: how many cores were busy in all, and the busiest
// core's busy and softirq fractions.
struct HostCpu {
  double cores = 0;
  double core_busy_max = 0;
  double core_softirq_max = 0;
};

HostCpu
host_cpu(const std::vector<CoreTicks> &before,
         const std::vector<CoreTicks> &after)
{
  HostCpu out;
  for (size_t i = 0; i < before.size() && i < after.size(); i++) {
    const int64_t total = after[i].total - before[i].total;
    if (total <= 0) {
      continue;
    }
    const double busy = static_cast<double>(after[i].busy - before[i].busy) /
                        static_cast<double>(total);
    const double softirq =
        static_cast<double>(after[i].softirq - before[i].softirq) /
        static_cast<double>(total);
    out.cores += busy;
    out.core_busy_max = std::max(out.core_busy_max, busy);
    out.core_softirq_max = std::max(out.core_softirq_max, softirq);
  }
  return out;
}

constexpr size_t kNumDbRpcKinds = static_cast<size_t>(DbRpcKind::kCount);

// hog-db's executor-RPC handler timing (hog_db/db_metrics.h), by kind.
struct DbRpcStats {
  std::array<int64_t, kNumDbRpcKinds> count{};
  std::array<int64_t, kNumDbRpcKinds> wall_ns{};
  std::array<int64_t, kNumDbRpcKinds> cpu_ns{};

  DbRpcStats operator-(const DbRpcStats &o) const
  {
    DbRpcStats d;
    for (size_t i = 0; i < kNumDbRpcKinds; i++) {
      d.count[i] = count[i] - o.count[i];
      d.wall_ns[i] = wall_ns[i] - o.wall_ns[i];
      d.cpu_ns[i] = cpu_ns[i] - o.cpu_ns[i];
    }
    return d;
  }
  double wall_avg_us(DbRpcKind k) const
  {
    const auto i = static_cast<size_t>(k);
    return count[i] > 0 ? static_cast<double>(wall_ns[i]) / 1000.0 /
                              static_cast<double>(count[i])
                        : 0.0;
  }
  double cpu_avg_us(DbRpcKind k) const
  {
    const auto i = static_cast<size_t>(k);
    return count[i] > 0 ? static_cast<double>(cpu_ns[i]) / 1000.0 /
                              static_cast<double>(count[i])
                        : 0.0;
  }
  int64_t total_count() const
  {
    return std::accumulate(count.begin(), count.end(), int64_t{0});
  }
  int64_t total_wall_ns() const
  {
    return std::accumulate(wall_ns.begin(), wall_ns.end(), int64_t{0});
  }
  int64_t total_cpu_ns() const
  {
    return std::accumulate(cpu_ns.begin(), cpu_ns.end(), int64_t{0});
  }
};

DbRpcStats
parse_db_rpc_stats(std::string_view text)
{
  DbRpcStats out;
  for (size_t i = 0; i < kNumDbRpcKinds; i++) {
    const std::string prefix = std::format("db_rpc_{}_", kDbRpcKindNames[i]);
    out.count[i] = metric_value(text, prefix + "count");
    out.wall_ns[i] = metric_value(text, prefix + "wall_ns");
    out.cpu_ns[i] = metric_value(text, prefix + "cpu_ns");
  }
  return out;
}

// One process's threads from its /threadstat route: tid -> (name, user plus
// system ticks).
using ThreadTicks = std::map<int64_t, std::pair<std::string, int64_t>>;

ThreadTicks
parse_threadstat(std::string_view text)
{
  ThreadTicks out;
  std::istringstream in{std::string(text)};
  std::string line;
  while (std::getline(in, line)) {
    // "<tid> <name> <ticks>"; the name may hold spaces, the ends do not.
    const size_t first = line.find(' ');
    const size_t last = line.rfind(' ');
    if (first == std::string::npos || last <= first) {
      continue;
    }
    out[std::stoll(line.substr(0, first))] = {
        line.substr(first + 1, last - first - 1),
        std::stoll(line.substr(last + 1))};
  }
  return out;
}

// Which threads of one process were busy across a window, as fractions of
// one core: the x23 poll thread (the busiest, if there are several), the
// x23 workers summed and at their busiest, and the three busiest threads of
// any kind as `name:fraction`.
struct ThreadLoad {
  double poll_max = 0;
  size_t n_poll = 0;
  double workers = 0;
  double worker_max = 0;
  std::string top;
};

ThreadLoad
thread_load(const ThreadTicks &before, const ThreadTicks &after, double wall_s)
{
  ThreadLoad out;
  if (wall_s <= 0) {
    return out;
  }
  const double ticks_per_s = static_cast<double>(::sysconf(_SC_CLK_TCK));
  std::vector<std::pair<double, std::string>> busy;
  for (const auto &[tid, entry] : after) {
    const auto &[name, ticks] = entry;
    const auto it = before.find(tid);
    const int64_t delta = ticks - (it == before.end() ? 0 : it->second.second);
    const double frac = static_cast<double>(delta) / ticks_per_s / wall_s;
    busy.emplace_back(frac, name);
    if (name == "x23-poll") {
      out.n_poll++;
      out.poll_max = std::max(out.poll_max, frac);
    }
    else if (name == "x23-worker") {
      out.workers += frac;
      out.worker_max = std::max(out.worker_max, frac);
    }
  }
  std::ranges::sort(busy, std::greater{});
  for (size_t i = 0; i < busy.size() && i < 3; i++) {
    out.top += std::format("{}{}:{:.2f}", i == 0 ? "" : ",", busy[i].second,
                           busy[i].first);
  }
  return out;
}

// Everything the runner scrapes from hog-db and the executors at one moment.
struct Sample {
  uint64_t wall_ns = 0;
  double db_cpu_s = 0;
  std::vector<double> exec_cpu_s;
  std::vector<CoreTicks> db_host;
  std::vector<std::vector<CoreTicks>> exec_hosts;
  DbRpcStats db_rpc;
  ThreadTicks db_threads;
  std::vector<ThreadTicks> exec_threads;
};

// What two samples say about the window between them. Cores are averages
// over the window: process CPU time (`db_cores`, `exec_cores_max` for the
// busiest executor), the hosts' per-core /proc/stat, and hog-db handler time
// (`db_handler_cores`: wall time inside handlers per second, i.e. how many
// connection readers were occupied on average).
struct LoadStats {
  double db_cores = 0;
  double exec_cores_max = 0;
  HostCpu db_host;
  HostCpu exec_host; // field-wise max over executor hosts
  DbRpcStats db_rpc;
  double db_handler_cores = 0;
  double db_handler_cpu_cores = 0;
  ThreadLoad db_threads;
  // The executor whose poll thread was busiest.
  ThreadLoad exec_threads;
};

LoadStats
load_stats(const Sample &before, const Sample &after)
{
  const double wall_s =
      static_cast<double>(after.wall_ns - before.wall_ns) / 1e9;
  if (wall_s <= 0) {
    return {};
  }
  LoadStats out{.db_cores = (after.db_cpu_s - before.db_cpu_s) / wall_s,
                .db_host = host_cpu(before.db_host, after.db_host),
                .db_rpc = after.db_rpc - before.db_rpc};
  for (size_t i = 0;
       i < before.exec_cpu_s.size() && i < after.exec_cpu_s.size(); i++) {
    out.exec_cores_max =
        std::max(out.exec_cores_max,
                 (after.exec_cpu_s[i] - before.exec_cpu_s[i]) / wall_s);
  }
  for (size_t i = 0;
       i < before.exec_hosts.size() && i < after.exec_hosts.size(); i++) {
    const HostCpu h = host_cpu(before.exec_hosts[i], after.exec_hosts[i]);
    out.exec_host.cores = std::max(out.exec_host.cores, h.cores);
    out.exec_host.core_busy_max =
        std::max(out.exec_host.core_busy_max, h.core_busy_max);
    out.exec_host.core_softirq_max =
        std::max(out.exec_host.core_softirq_max, h.core_softirq_max);
  }
  out.db_threads = thread_load(before.db_threads, after.db_threads, wall_s);
  for (size_t i = 0;
       i < before.exec_threads.size() && i < after.exec_threads.size(); i++) {
    ThreadLoad t =
        thread_load(before.exec_threads[i], after.exec_threads[i], wall_s);
    if (i == 0 || t.poll_max > out.exec_threads.poll_max) {
      out.exec_threads = std::move(t);
    }
  }
  out.db_handler_cores =
      static_cast<double>(out.db_rpc.total_wall_ns()) / 1e9 / wall_s;
  out.db_handler_cpu_cores =
      static_cast<double>(out.db_rpc.total_cpu_ns()) / 1e9 / wall_s;
  return out;
}

std::string
format_load(const LoadStats &l)
{
  return std::format(
      "db_cores={:.1f} exec_cores_max={:.1f} db_host_cores={:.1f} "
      "db_core_busy_max={:.2f} db_core_si_max={:.2f} "
      "exec_host_cores_max={:.1f} exec_core_busy_max={:.2f} "
      "exec_core_si_max={:.2f} db_rpcs={} db_handler_cores={:.2f} "
      "db_handler_cpu_cores={:.2f} snap_wall_us={:.1f} snap_cpu_us={:.1f} "
      "push_wall_us={:.1f} push_cpu_us={:.1f} commit_wall_us={:.1f} "
      "commit_cpu_us={:.1f} other_wall_us={:.1f} db_poll_busy={:.2f} "
      "db_n_poll={} db_workers={:.1f} db_worker_max={:.2f} db_top={} "
      "exec_poll_busy={:.2f} exec_n_poll={} exec_workers={:.1f} "
      "exec_worker_max={:.2f} exec_top={}",
      l.db_cores, l.exec_cores_max, l.db_host.cores, l.db_host.core_busy_max,
      l.db_host.core_softirq_max, l.exec_host.cores, l.exec_host.core_busy_max,
      l.exec_host.core_softirq_max, l.db_rpc.total_count(), l.db_handler_cores,
      l.db_handler_cpu_cores, l.db_rpc.wall_avg_us(DbRpcKind::kGetSnapshotItem),
      l.db_rpc.cpu_avg_us(DbRpcKind::kGetSnapshotItem),
      l.db_rpc.wall_avg_us(DbRpcKind::kTryPushSubtransaction),
      l.db_rpc.cpu_avg_us(DbRpcKind::kTryPushSubtransaction),
      l.db_rpc.wall_avg_us(DbRpcKind::kCommitTransaction),
      l.db_rpc.cpu_avg_us(DbRpcKind::kCommitTransaction),
      l.db_rpc.wall_avg_us(DbRpcKind::kOther), l.db_threads.poll_max,
      l.db_threads.n_poll, l.db_threads.workers, l.db_threads.worker_max,
      l.db_threads.top, l.exec_threads.poll_max, l.exec_threads.n_poll,
      l.exec_threads.workers, l.exec_threads.worker_max, l.exec_threads.top);
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
      std::vector<LoadStats> loads;
      for (uint32_t trial = 0; trial < params_.n_trials; trial++) {
        const TrialResult r = run_trial(s, trial);
        makespans.push_back(r.makespan_ns);
        if (s.n_heavy > 0) {
          heavy_maxes.push_back(r.heavy_max_ns);
        }
        read_rpc_us.push_back(r.reads.rpc_avg_us());
        read_hop_us.push_back(r.reads.hop_avg_us());
        loads.push_back(r.load);
      }
      std::ranges::sort(makespans);
      std::ranges::sort(heavy_maxes);
      std::ranges::sort(read_rpc_us);
      std::ranges::sort(read_hop_us);
      auto median = [&loads](auto field) {
        std::vector<double> v;
        v.reserve(loads.size());
        for (const LoadStats &l : loads) {
          v.push_back(field(l));
        }
        std::ranges::sort(v);
        return v[v.size() / 2];
      };
      std::println(
          "makespan_summary: scenario={} trials={} "
          "makespan_min_us={} makespan_median_us={} "
          "makespan_max_us={} heavy_max_median_us={} "
          "read_rpc_avg_median_us={:.1f} "
          "read_hop_avg_median_us={:.1f} db_cores_median={:.1f} "
          "exec_cores_max_median={:.1f} db_core_si_max_median={:.2f} "
          "exec_core_si_max_median={:.2f} db_handler_cores_median={:.2f} "
          "snap_wall_us_median={:.1f} snap_cpu_us_median={:.1f} "
          "commit_wall_us_median={:.1f} db_poll_busy_median={:.2f} "
          "exec_poll_busy_median={:.2f}",
          s.name, makespans.size(), makespans.front() / 1000,
          makespans[makespans.size() / 2] / 1000, makespans.back() / 1000,
          heavy_maxes.empty() ? 0 : heavy_maxes[heavy_maxes.size() / 2] / 1000,
          read_rpc_us[read_rpc_us.size() / 2],
          read_hop_us[read_hop_us.size() / 2],
          median([](const LoadStats &l) { return l.db_cores; }),
          median([](const LoadStats &l) { return l.exec_cores_max; }),
          median([](const LoadStats &l) { return l.db_host.core_softirq_max; }),
          median(
              [](const LoadStats &l) { return l.exec_host.core_softirq_max; }),
          median([](const LoadStats &l) { return l.db_handler_cores; }),
          median([](const LoadStats &l) {
            return l.db_rpc.wall_avg_us(DbRpcKind::kGetSnapshotItem);
          }),
          median([](const LoadStats &l) {
            return l.db_rpc.cpu_avg_us(DbRpcKind::kGetSnapshotItem);
          }),
          median([](const LoadStats &l) {
            return l.db_rpc.wall_avg_us(DbRpcKind::kCommitTransaction);
          }),
          median([](const LoadStats &l) { return l.db_threads.poll_max; }),
          median([](const LoadStats &l) { return l.exec_threads.poll_max; }));
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

  // Fetches a path from hog-db or an executor, warning when it cannot.
  static std::optional<std::string> scrape(const std::string &ip, uint16_t port,
                                           std::string_view path)
  {
    auto text = fetch_metrics(ip, port, path);
    if (!text) {
      warn("could not scrape {} from {}:{}", path, ip, port);
    }
    return text;
  }

  static int64_t scrape_available_cpus(const std::string &ip, uint16_t port)
  {
    auto text = scrape(ip, port, "/metrics");
    return text ? metric_value(*text, "process_available_cpus") : 0;
  }

  // Whatever cannot be scraped reads as zero, so its share of a trial's
  // load comes out zero or negative.
  Sample scrape_sample()
  {
    Sample sample{.wall_ns = x23::fastclock::get_ns()};
    if (auto text = scrape(cluster_.db_ip(), kBenchDbMetricsPort, "/metrics")) {
      sample.db_cpu_s = metric_double(*text, "process_cpu_seconds_total");
      sample.db_rpc = parse_db_rpc_stats(*text);
    }
    if (auto text =
            scrape(cluster_.db_ip(), kBenchDbMetricsPort, "/procstat")) {
      sample.db_host = parse_procstat(*text);
    }
    if (auto text =
            scrape(cluster_.db_ip(), kBenchDbMetricsPort, "/threadstat")) {
      sample.db_threads = parse_threadstat(*text);
    }
    for (const std::string &ip : cluster_.exec_ips()) {
      auto metrics = scrape(ip, kBenchExecMetricsPort, "/metrics");
      sample.exec_cpu_s.push_back(
          metrics ? metric_double(*metrics, "process_cpu_seconds_total") : 0);
      auto procstat = scrape(ip, kBenchExecMetricsPort, "/procstat");
      sample.exec_hosts.push_back(procstat ? parse_procstat(*procstat)
                                           : std::vector<CoreTicks>{});
      auto threads = scrape(ip, kBenchExecMetricsPort, "/threadstat");
      sample.exec_threads.push_back(threads ? parse_threadstat(*threads)
                                            : ThreadTicks{});
    }
    return sample;
  }

  // Reads from hog-db directly, the way executors do but with nothing else
  // in the path, at increasing numbers of reads in flight over as many
  // connections as the executors open between them. Each level prints a
  // `probe:` line. Run inside its own (empty) block, after the scenarios.
  void run_probe()
  {
    const size_t n_connections =
        static_cast<size_t>(db_connection_count()) * cluster_.num_executors();
    std::vector<db_probe_client_t> clients;
    clients.reserve(n_connections);
    for (size_t i = 0; i < n_connections; i++) {
      clients.push_back(make_rpc_multiclient<DatabaseToExecutorInterfaceV1>(
          cluster_.db_exec_addr(), false, *x23::single_bucket_map));
    }

    open_block(cluster_.db_sched_client(), cluster_.event_sched_client(),
               exec_clients_, parent_block_number_, current_block_number_,
               next_block_height());
    const std::array<size_t, 5> levels = {1, n_connections, 10 * n_connections,
                                          30 * n_connections,
                                          100 * n_connections};
    for (const size_t inflight : levels) {
      const int64_t n_reads = inflight == 1 ? 2000 : 300'000;
      auto [async, cb] = x23::make_async<void>();
      Probe p{.block = current_block_number_,
              .address = reads_addr_,
              .remaining = n_reads,
              .live_coros = inflight,
              .latency_ns = std::vector<std::vector<uint64_t>>(inflight),
              .done = &cb};
      const Sample before = scrape_sample();
      for (size_t i = 0; i < inflight; i++) {
        probe_coro(clients[i % clients.size()], &p, i);
      }
      async();
      const Sample after = scrape_sample();

      std::vector<uint64_t> latency;
      latency.reserve(static_cast<size_t>(n_reads));
      for (const auto &v : p.latency_ns) {
        latency.insert(latency.end(), v.begin(), v.end());
      }
      std::ranges::sort(latency);
      const double wall_s =
          static_cast<double>(after.wall_ns - before.wall_ns) / 1e9;
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
                   "lat_p99_us={:.1f} unexpected_errors={} {}",
                   inflight, std::min(inflight, n_connections), latency.size(),
                   wall_s * 1e3, static_cast<double>(latency.size()) / wall_s,
                   latency.empty()
                       ? 0.0
                       : sum_ns / 1000.0 / static_cast<double>(latency.size()),
                   pct_us(0.5), pct_us(0.99), p.unexpected_errors.load(),
                   format_load(load_stats(before, after)));
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
    ReadStats reads;
    LoadStats load;
  };

  TrialResult run_trial(const Scenario &s, uint32_t trial)
  {
    const Block block = make_block(s, trial);
    const ReadStats reads_before = scrape_read_stats();
    const Sample before = scrape_sample();
    const std::vector<UnitTiming> timings = run_block(block);
    const LoadStats load = load_stats(before, scrape_sample());
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
                 "read_hop_avg_us={:.1f} {}",
                 s.name, trial, cluster_.num_executors(), block.txs.size(),
                 block.units.size(), makespan / 1000, pct_us(light_lat, 0.5),
                 pct_us(light_lat, 0.99), tx_class_name(s.heavy_class),
                 heavy_order_name(s.order), pct_us(heavy_lat, 0.5),
                 pct_us(heavy_lat, 1.0), heavy_done / 1000, reads.count,
                 reads.rpc_avg_us(), reads.hop_avg_us(), format_load(load));
    std::fflush(stdout);
    return {.makespan_ns = makespan,
            .heavy_max_ns = heavy_lat.empty() ? 0 : heavy_lat.back(),
            .reads = reads,
            .load = load};
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
