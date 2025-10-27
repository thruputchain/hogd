#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "ci_server.hh"
#include "db_sched.hh"
#include "event_rpc.hh"
#include "exec_sched.hh"
#include "genesis.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/bench/control_client.h"
#include "hogd/hogexec/runtime/test_util/test_event_aggregator.h"
#include "hogd/init.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/version.h"
#include "loadgen.hh"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "sched_event.hh"
#include "sched_exec.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/fastclock.h"
#include "x23/format.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/srpc.h"
#include "x23/vec.h"

namespace hogd {

class ControlServerHandle {
public:
  ControlServerHandle(std::string const &server_addr)
    : ci_client(connect_to_control_server(server_addr,
                                          std::to_string(kControlServerPort)))
  {}

  ControlClientWrapper ci_client;

  ~ControlServerHandle()
  {
    try {
      ci_client.stop_server();
    } catch (...) {
      panic("Failed to stop control server");
    }
  }
};

class SchedExecTest {
public:
  using rpc_service = SchedExec1;

  SchedExecTest() = default;

  // Disable copy and move.
  SchedExecTest(const SchedExecTest &) = delete;
  SchedExecTest &operator=(const SchedExecTest &) = delete;
  SchedExecTest(SchedExecTest &&) = delete;
  SchedExecTest &operator=(SchedExecTest &&) = delete;

  x23::Async<IntroExecResult> schedexec_intro_exec(const ExecCard &card)
  {
    info("executor with card {} connected", x23::show(card));

    auto r = default_coropoll().pool_.async(
        [](const ExecCard &card, std::mutex &mtx, std::condition_variable &cv,
           std::vector<ExecCard> &exec_cards, uint16_t &exec_id,
           const EventCard &event_card,
           const x23::Vec<ShardCard> &shard_cards) {
          info("doing lambda for schedexec_intro_exec");
          std::unique_lock lk(mtx);
          exec_cards.push_back(card);
          info("pushed card {} to exec_cards", exec_cards.size());
          cv.notify_all();

          cv.wait(lk, [&event_card] { return event_card != EventCard{}; });
          info("event_card is not empty");
          cv.wait(lk, [&shard_cards] { return !shard_cards.empty(); });
          info("shard_cards is not empty");

          return IntroExecResult{.exec_id = exec_id++,
                                 .shards = shard_cards,
                                 .event = event_card};
        },
        card, std::ref(mtx_), std::ref(cv_), std::ref(exec_cards_),
        std::ref(exec_id_), std::ref(event_card_), std::ref(shard_cards_));

    co_return co_await r;
  }

  void register_event_card(EventCard card)
  {
    std::unique_lock lk(mtx_);
    event_card_ = std::move(card);
    cv_.notify_all();
  }

  void register_shard_cards(x23::Vec<ShardCard> cards)
  {
    std::unique_lock lk(mtx_);
    shard_cards_ = std::move(cards);
    cv_.notify_all();
  }

  std::vector<ExecCard> await_executor_cards(unsigned int num_cards)
  {
    std::unique_lock lk(mtx_);
    cv_.wait(lk, [this, num_cards] { return exec_cards_.size() == num_cards; });
    return std::move(exec_cards_);
  }

private:
  std::mutex mtx_;
  std::condition_variable cv_;
  std::vector<ExecCard> exec_cards_;
  uint16_t exec_id_ = 0;
  EventCard event_card_{};
  x23::Vec<ShardCard> shard_cards_;
};

static_assert(is_SchedExec1<SchedExecTest *, x23::MaybeAsync>);

class SchedDBTest {
public:
  using rpc_service = SchedDb1;

  SchedDBTest() = default;

  // Disable copy and move.
  SchedDBTest(const SchedDBTest &) = delete;
  SchedDBTest &operator=(const SchedDBTest &) = delete;
  SchedDBTest(SchedDBTest &&) = delete;
  SchedDBTest &operator=(SchedDBTest &&) = delete;

  void scheddb_intro_shard(const ShardCard &card)
  {
    std::unique_lock lk(mtx_);
    shard_cards_.push_back(card);
    cv_.notify_all();
  }

  static void scheddb_hogs_transferred(const HogsTransferred &hogs) {}

  x23::Vec<ShardCard> await_shard_cards(unsigned int num_cards)
  {
    std::unique_lock lk(mtx_);
    cv_.wait(lk,
             [this, num_cards] { return shard_cards_.size() == num_cards; });
    return shard_cards_;
  }

private:
  std::mutex mtx_;
  std::condition_variable cv_;
  x23::Vec<ShardCard> shard_cards_;
};

static_assert(is_SchedDb1<SchedDBTest *, std::type_identity_t>);

namespace {
auto
connect_to_event_sched_server(uint16_t port)
{
  return make_rpc_client<EventSched1>(x23::HostPort{"localhost", port}, false);
}

auto
connect_to_db_server(ShardCard const &card)
{
  x23::HostPort host_port;
  if (!host_port.parse(card.address_for_sched)) {
    throw std::runtime_error("Failed to parse db server address");
  }
  return make_srpc_client<DbSched1>(host_port, false);
}

auto
connect_to_exec_server(const ExecCard &card)
{
  x23::HostPort host_port;
  if (!host_port.parse(card.address)) {
    throw std::runtime_error("Failed to parse exec server address");
  }
  return make_rpc_client<ExecutorSched1>(host_port, false);
}

} // namespace

struct BlockValidationQueue {

  BlockValidationQueue(const auto &txs, size_t batch_size)
    : txs(txs), batch_size(batch_size)
  {}

  void reset_offset() { offset.store(0); }

  const std::span<const SignedTransaction> txs;
  const size_t batch_size;
  std::atomic<uint64_t> offset{0};

  std::span<const SignedTransaction> next_batch()
  {
    auto start = offset.fetch_add(batch_size);
    return {txs.begin() + std::min<size_t>(start, txs.size()),
            txs.begin() + std::min<size_t>(start + batch_size, txs.size())};
  }
};

namespace {

using exec_client_t =
    ExecutorSched1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;
using event_sched_client_t = decltype(connect_to_event_sched_server(0));
using db_sched_client_t = decltype(connect_to_db_server(ShardCard{}));

void
open_block(db_sched_client_t &db_sched_client,
           event_sched_client_t &event_sched_client,
           std::vector<exec_client_t> &exec_clients,
           uint64_t parent_block_number, uint64_t block_number)
{
  info("opening block {}", block_number);

  auto [async, cb] = x23::make_async<void>();
  std::atomic<size_t> n_remaining_execs = exec_clients.size();

  for (auto &exec_client : exec_clients) {
    [](auto &exec_client, uint64_t block_number,
       std::atomic<size_t> &n_remaining_execs, auto &cb) -> x23::Detached {
      co_await exec_client.executorsched_open_proposal_block(block_number);
      if (n_remaining_execs.fetch_sub(1, std::memory_order_relaxed) == 1) {
        cb();
      }
    }(exec_client, block_number, n_remaining_execs, cb);
  }

  auto event_async = event_sched_client.eventsched_open_block(block_number);
  auto res = db_sched_client.dbsched_create_snapshot(
      parent_block_number, block_number,
      BlockExecutionMode::kBlockExecutionModeProposal);
  if (res != DbSchedResult::DbSchedResultOK) {
    throw std::runtime_error("Failed to create snapshot");
  }

  async();
  event_async();
};

void
close_block(db_sched_client_t &db_sched_client,
            event_sched_client_t &event_sched_client,
            std::vector<exec_client_t> &exec_clients, uint64_t block_number)
{
  info("closing block {}", block_number);
  auto [async, cb] = x23::make_async<void>();
  std::atomic<size_t> n_remaining_execs = exec_clients.size();

  for (auto &exec_client : exec_clients) {
    [](auto &exec_client, uint64_t block_number,
       std::atomic<size_t> &n_remaining_execs, auto &cb) -> x23::Detached {
      co_await exec_client.executorsched_finish_block(block_number);
      if (n_remaining_execs.fetch_sub(1, std::memory_order_relaxed) == 1) {
        cb();
      }
    }(exec_client, block_number, n_remaining_execs, cb);
  }
  auto event_async = event_sched_client.eventsched_close_block(block_number);
  auto res = db_sched_client.dbsched_close_snapshot(block_number);
  if (res.result() != DbSchedResult::DbSchedResultOK) {
    throw std::runtime_error("Failed to close snapshot");
  }
  auto commit_res = db_sched_client.dbsched_commit_snapshot(block_number);
  if (commit_res.result() != DbSchedResult::DbSchedResultOK) {
    throw std::runtime_error("Failed to commit snapshot");
  }
  // It's ok for finish_block to overlap with db.close_block, because we already
  // synchronized waiting for all pending txs on the block to finish.
  async();
  event_async();
};

x23::Detached
connection_coro(exec_client_t &exec_client, BlockValidationQueue *queue,
                x23::CoroCB<void> &cb, int ncoro, BlockID block_number,
                std::atomic<size_t> &n_total_coros)
{

  auto coro = [](exec_client_t &exec_client, x23::CoroCB<void> &cb,
                 BlockValidationQueue *queue, BlockID block_number,
                 std::atomic<size_t> &n_total_coros) -> x23::Detached {
    co_await default_threadpool().bg();
    while (true) {
      auto batch = queue->next_batch();
      if (batch.empty()) {
        if (n_total_coros.fetch_sub(1, std::memory_order_relaxed) == 1) {
          cb();
        }
        co_return;
      }
      x23::Vec<SignedTransaction> batch_vec;
      batch_vec.reserve(batch.size());
      for (auto const &tx : batch) {
        batch_vec.push_back(tx);
      }

      auto r = co_await exec_client.executorsched_propose_batch(block_number,
                                                                batch_vec);
      assert(r.size() == batch.size());
      for (auto const &result : r) {
        if (result.result() != ExecResult::ExecResultOK) {
          throw std::runtime_error("Failed to propose batch");
        }
      }
    }
  };

  for (int i = 0; i < ncoro; i++) {
    coro(exec_client, cb, queue, block_number, n_total_coros);
  }
  co_return;
};

void
run_block(x23::Vec<SignedTransaction> const &txs,
          db_sched_client_t &db_sched_client,
          event_sched_client_t &event_sched_client,
          std::vector<exec_client_t> &exec_clients,
          size_t n_coros_per_connection, size_t n_connections,
          size_t batch_size, uint64_t parent_block_number,
          uint64_t current_block_number)
{

  auto start_time = x23::fastclock::get_ns();

  std::atomic<size_t> n_total_coros = n_coros_per_connection * n_connections;
  open_block(db_sched_client, event_sched_client, exec_clients,
             parent_block_number, current_block_number);

  BlockValidationQueue block_validation_queue(txs, batch_size);

  auto start_coros_time = x23::fastclock::get_ns();

  // make sure any destructors are run during the measured inner loop
  {
    auto [async, cb] = x23::make_async<void>();
    for (auto &exec_client : exec_clients) {
      connection_coro(exec_client, &block_validation_queue, cb,
                      n_coros_per_connection, current_block_number,
                      n_total_coros);
    }
    async();
  }

  auto end_coros_time = x23::fastclock::get_ns();
  double elapsed_time = end_coros_time - start_coros_time;
  auto inner_tps = txs.size() * 1'000'000'000.0 / elapsed_time;
  close_block(db_sched_client, event_sched_client, exec_clients,
              current_block_number);

  auto end_block_time = x23::fastclock::get_ns();
  std::println("open_time {} middle_time {} close_time {} inner tps: {}",
               start_coros_time - start_time, end_coros_time - start_coros_time,
               end_block_time - end_coros_time, inner_tps);
}

void
run_multiexec_bench(const std::vector<std::string> &server_addrs,
                    unsigned int num_accounts,
                    unsigned int n_connections_per_executor,
                    unsigned int n_coros_per_connection,
                    unsigned int batch_size, unsigned int num_txs_per_block)
{
  std::vector<ControlServerHandle> control_server_handles;
  control_server_handles.reserve(server_addrs.size());
  for (const auto &server_addr : server_addrs) {
    control_server_handles.emplace_back(server_addr);
  }

  if (server_addrs.size() < 2) {
    // 1 db and the rest executors
    throw std::runtime_error("At least 2 servers are required");
  }

  RpcServer<SchedExecTest> sched_exec_test;
  uint16_t sched_exec_port = sched_exec_test.start_server(0);

  RpcServer<SchedDBTest> sched_db_test;
  uint16_t sched_db_port = sched_db_test.start_server(0);

  TestEventAggregator event_server;
  TestEventAggregatorExecServer event_server_exec_server =
      TestEventAggregatorExecServer(event_server);
  TestEventAggregatorSchedServer event_server_sched_server =
      TestEventAggregatorSchedServer(event_server);

  uint16_t event_server_exec_port = event_server_exec_server.start_server(0);
  uint16_t event_server_sched_port = event_server_sched_server.start_server(0);

  std::string hostname;
  CHECK_EQ(get_hostname(hostname), 0);

  sched_exec_test.impl_.register_event_card(EventCard{
      .protocol = version::kHogdProtocol,
      .upid = {},
      .address_for_exec =
          std::format("{}:{}", hostname, event_server_exec_port).c_str(),
      .address_for_sched =
          std::format("{}:{}", hostname, event_server_sched_port).c_str()});

  auto work_dir = control_server_handles[0].ci_client.query_work_dir();
  if (work_dir.error_code() != CIServerErrorCode::kCIServerErrorCodeOK) {
    throw std::runtime_error("Failed to query work dir");
  }

  for (auto &control_server_handle : control_server_handles) {
    control_server_handle.ci_client.clear_work_dir();
  }

  std::string work_dir_str = std::format("{}", work_dir.work_dir());
  info("using work dir: {}", work_dir_str);

  size_t server_idx = 0;
  control_server_handles[server_idx].ci_client.start_server(
      "hogd/hogdb/hogdb", {work_dir_str, "10000000000",
                           std::format("{}:{}", hostname, sched_db_port)});
  server_idx++;

  for (; server_idx < server_addrs.size(); server_idx++) {
    control_server_handles[server_idx].ci_client.start_server(
        "hogd/hogexec/hogexec",
        {std::format("{}:{}", hostname, sched_exec_port), "-n", "1024"});
  }

  auto db_cards = sched_db_test.impl_.await_shard_cards(1);
  sched_exec_test.impl_.register_shard_cards(db_cards);
  auto exec_cards =
      sched_exec_test.impl_.await_executor_cards(server_addrs.size() - 1);

  auto event_sched_client =
      connect_to_event_sched_server(event_server_sched_port);
  assert(db_cards.size() == 1);
  auto db_sched_client = connect_to_db_server(db_cards[0]);

  auto await_ready = [&](const ExecCard &exec_card) {
    auto exec_client = connect_to_exec_server(exec_card);
    while (true) {
      auto res = exec_client.executorsched_ready()();
      if (res != ExecResult::ExecResultOK) {
        warn_every_n_sec(1.0, "Executor is not ready, retrying...");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      break;
    }
  };
  for (auto &exec_card : exec_cards) {
    await_ready(exec_card);
  }

  auto write_genesis_block = [&](const GenesisBlock &genesis_block) {
    auto res = db_sched_client.dbsched_load_genesis_block(genesis_block);
    if (res != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error("Failed to write genesis block");
    }
  };

  uint64_t parent_block_number = 0;
  uint64_t current_block_number = 1;

  ExperimentConfig config(ExperimentType::kExperimentTypeERC20Payments);
  config.erc20_payments() = {
      .num_accounts = num_accounts,
  };

  LoadgenRpc loadgen;
  auto loadgen_result = loadgen.generate_experiment(config);
  if (loadgen_result.error() != LoadgenRPCError::kLoadgenRPCErrorOK) {
    throw std::runtime_error("Failed to generate loadgen experiment");
  }
  auto genesis_block = loadgen_result.success().genesis_block;
  write_genesis_block(genesis_block);

  const size_t n_connections = n_connections_per_executor * exec_cards.size();

  std::vector<exec_client_t> coro_exec_clients;
  coro_exec_clients.reserve(n_connections);
  for (size_t i = 0; i < n_connections; i++) {
    coro_exec_clients.push_back(
        connect_to_exec_server(exec_cards[i % exec_cards.size()]));
  }

  for (auto const &init_block : loadgen_result.success().initialization_txs) {
    run_block(init_block, db_sched_client, event_sched_client,
              coro_exec_clients, n_coros_per_connection, n_connections,
              batch_size, parent_block_number, current_block_number);
    parent_block_number = current_block_number;
    current_block_number++;
  }

  info("done initialization txs");

  auto work_txs = loadgen.extend_experiment(loadgen_result.success().context_id,
                                            num_txs_per_block, 1000000);
  if (work_txs.error() != LoadgenRPCError::kLoadgenRPCErrorOK) {
    throw std::runtime_error("Failed to extend loadgen experiment");
  }

  assert(work_txs.success().work_txs.size() == num_txs_per_block);

  for (size_t i = 0; i < 5; i++) {
    std::println("Start trial {} with txs={} nexecutors={} nconnections={} "
                 "ncoros_per_connection={} nacc={} "
                 "batch_size={}",
                 i, num_txs_per_block, exec_cards.size(), n_connections,
                 n_coros_per_connection, num_accounts, batch_size);
    auto const &work = work_txs.success().work_txs;
    auto ts = x23::fastclock::get_ns();
    run_block(work, db_sched_client, event_sched_client, coro_exec_clients,
              n_coros_per_connection, n_connections, batch_size,
              parent_block_number, current_block_number);
    auto end_time = x23::fastclock::get_ns();
    double elapsed_time = end_time - ts;
    auto tps = num_txs_per_block * 1'000'000'000.0 / elapsed_time;
    std::println("time taken: {} ns for txs={} nexecutors={} nconnections={} "
                 "ncoros_per_connection={} nacc={} "
                 "batch_size={} tps={}",
                 elapsed_time, num_txs_per_block, exec_cards.size(),
                 n_connections, n_coros_per_connection, num_accounts,
                 batch_size, tps);
    info("time taken: {} ns for txs={} nexecutors={} nconnections={} "
         "ncoros_per_connection={} nacc={} "
         "batch_size={} tps={}",
         elapsed_time, num_txs_per_block, exec_cards.size(), n_connections,
         n_coros_per_connection, num_accounts, batch_size, tps);
    parent_block_number = current_block_number;
    current_block_number++;
  }
}

} // namespace

} // namespace hogd

namespace {
void
usage(char *argv[])
{
  std::println("Usage: {} -s <server_addr, repeated> -a "
               "<num_accounts>",
               argv[0]);
  exit(EXIT_FAILURE);
}
} // namespace
int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;

  std::vector<std::string> server_addrs;
  std::optional<int> num_accounts;

  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "s:a:")) != -1) {
    switch (opt) {
    case 's':
      server_addrs.emplace_back(optarg);
      break;
    case 'a':
      num_accounts = std::stoi(optarg);
      break;
    default:
      usage(argv);
    }
  }
  // NOLINTEND(misc-include-cleaner)
  if (server_addrs.empty() || !num_accounts) {
    usage(argv);
  }

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  hogd::run_multiexec_bench(server_addrs, *num_accounts, 10, 100, 1, 1'000'000);
  hogd::run_multiexec_bench(server_addrs, *num_accounts, 10, 100, 5, 1'000'000);
  hogd::run_multiexec_bench(server_addrs, *num_accounts, 10, 100, 10,
                            1'000'000);

  // NOLINTEND(bugprone-unchecked-optional-access)

  std::fflush(stdout);
  return 0;
}
