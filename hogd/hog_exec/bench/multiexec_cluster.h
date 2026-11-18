#pragma once

// The multiexec bench topology: hog-event, hog-db, hog-indexer and N
// executors started on CI control servers, with this process standing in for
// the mempool. The mock scheduler services drop what the mempool would act on
// (e.g. native-tokens-transferred chunks), so none of the mempool's own
// accounting runs and a runner measures the executors and HogDb alone. Shared
// by multiexec_runner and makespan_runner.

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <print>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ci_server.hh"
#include "consensus.hh"
#include "db_sched.hh"
#include "event_rpc.hh"
#include "exec_result.hh"
#include "exec_sched.hh"
#include "genesis.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/bench/common_bench_state.h"
#include "hogd/hog_exec/bench/control_client.h"
#include "hogd/hog_exec/bench/indexer_latency_probe.h"
#include "hogd/hog_exec/bench/mock_scheduler.h"
#include "hogd/hog_mempool/config_hash.h"
#include "network.hh"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "sched_event.hh"
#include "sched_exec.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/fastclock.h"
#include "x23/format.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/srpc.h"
#include "x23/vec.h"

namespace hogd {

// Must be the height the block will actually commit, since the executor
// cross-checks it against the tip. The timepoint is a stand-in.
constexpr BlockExecutionContext
test_block_execution(uint64_t block_height)
{
  return BlockExecutionContext{
      .block_height = static_cast<BlockHeight>(block_height),
      .block_timepoint = 1,
  };
}

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
          info("shard_cards is not empty: returning IntroExecResult");

          return IntroExecResult{.exec_id = exec_id++,
                                 .shards = x23::XdrVec<ShardCard>{shard_cards},
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
    info("awaiting {} executor cards", num_cards);
    std::unique_lock lk(mtx_);
    cv_.wait(lk, [this, num_cards] { return exec_cards_.size() == num_cards; });
    info("got {} executor cards", exec_cards_.size());
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

inline auto
connect_to_event_sched_server(const ServerAddress &address)
{
  x23::HostPort host_port;
  if (!host_port.parse(address)) {
    panic("Failed to parse event sched server address");
  }
  auto client =
      make_rpc_client<EventSched1>(host_port, false,
                                   kEventSchedulerRpcMaxMessageSize)();
  if (!client)
    panic("connect_to_event_sched_server failed: {}",
          client.error().error_code().message());
  return std::move(*client);
}

inline auto
connect_to_db_server(ShardCard const &card)
{
  x23::HostPort host_port;
  if (!host_port.parse(card.address_for_sched)) {
    panic("Failed to parse db server address");
  }
  return make_srpc_client<DbSched1>(host_port, false);
}

inline auto
connect_to_exec_server(const ExecCard &card,
                       x23::CoroPoll &cp = default_coropoll())
{
  x23::HostPort host_port;
  if (!host_port.parse(card.address)) {
    panic("Failed to parse exec server address");
  }
  auto client = make_rpc_client<ExecutorSched1>(host_port, cp, false)();
  if (!client)
    panic("connect_to_exec_server failed: {}",
          client.error().error_code().message());
  return std::move(*client);
}

// hog-event defaults its indexer port to 0 (ephemeral), which leaves the
// indexer with no address to dial. The bench owns its hosts outright, so
// fixed ports are simpler here than plumbing the chosen port back out of the
// event server's startup log.
inline constexpr uint16_t kBenchEventIndexerPort = 8092;
inline constexpr uint16_t kBenchIndexerBlockExplorerPort = 8093;

using exec_client_t =
    ExecutorSched1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;
using bench_event_sched_client_t =
    decltype(connect_to_event_sched_server("address"));
using bench_db_sched_client_t = decltype(connect_to_db_server(ShardCard{}));

inline void
open_block(bench_db_sched_client_t &db_sched_client,
           bench_event_sched_client_t &event_sched_client,
           std::vector<exec_client_t> &exec_clients,
           uint64_t parent_block_number, uint64_t block_number,
           BlockHeight block_height)
{
  info("opening block {}", block_number);
  auto start_time = x23::fastclock::now();

  auto [async, cb] = x23::make_async<void>();
  std::atomic<size_t> n_remaining_execs = exec_clients.size();

  for (auto &exec_client : exec_clients) {
    [](auto &exec_client, uint64_t block_number, BlockHeight block_height,
       std::atomic<size_t> &n_remaining_execs, auto &cb) -> x23::Detached {
      co_await exec_client.executorsched_open_proposal_block(
          block_number, test_block_execution(block_height));
      if (n_remaining_execs.fetch_sub(1, std::memory_order_relaxed) == 1) {
        cb();
      }
    }(exec_client, block_number, block_height, n_remaining_execs, cb);
  }

  auto event_async = event_sched_client.eventsched_open_block(block_number);
  auto issue_time = x23::fastclock::now();
  auto res = db_sched_client.dbsched_create_snapshot(
      parent_block_number, block_number,
      BlockExecutionMode::kBlockExecutionModeProposal);
  if (res != DbSchedResult::DbSchedResultOK) {
    panic("Failed to create snapshot");
  }
  auto create_time = x23::fastclock::now();
  async();
  event_async();
  auto done_time = x23::fastclock::now();

  std::println("opening: issue_time {} create_time {} done_time {}",
               (issue_time - start_time).count(),
               (create_time - issue_time).count(),
               (done_time - create_time).count());
}

// [prev_value] chains the committed ConsensusValues: it enters holding the
// previous block's committed value (the mock scheduler's genesis tip before
// the first block) and leaves holding this block's.
inline void
close_block(bench_db_sched_client_t &db_sched_client,
            bench_event_sched_client_t &event_sched_client,
            std::vector<exec_client_t> &exec_clients, uint64_t block_number,
            ConsensusValue &prev_value, IndexerLatencyProbe *indexer_probe)
{
  auto start_time = x23::fastclock::now();
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
  const BlockHeight block_height = prev_value.proposal_header.block_height + 1;
  auto event_async =
      event_sched_client.eventsched_close_block(block_number, block_height);
  auto issue_time = x23::fastclock::now();
  auto res = db_sched_client.dbsched_close_snapshot(block_number);
  if (res.result() != DbSchedResult::DbSchedResultOK) {
    panic("Failed to close snapshot");
  }
  auto close_time = x23::fastclock::now();
  ConsensusValue value{};
  value.proposal_header.block_height = block_height;
  auto commit_res =
      db_sched_client.dbsched_commit_snapshot(block_number, value);
  if (commit_res.result() != DbSchedResult::DbSchedResultOK) {
    panic("Failed to commit snapshot");
  }
  auto commit_time = x23::fastclock::now();
  // It's ok for finish_block to overlap with db.close_block, because we already
  // synchronized waiting for all pending txs on the block to finish.
  async();
  auto event_result = event_async();
  if (event_result.error() != RPCError::kRPCNone) {
    panic("Failed to close event block");
  }
  auto done_time = x23::fastclock::now();
  // Commit the event block the way a real finalization does, so the measured
  // lifecycle covers the durable indexer publication and not just close. The
  // finalized value chains prev_hash off the previous committed value and
  // carries the event root close returned; the event server validated its
  // origin against the mock scheduler's genesis tip at registration.
  ConsensusValue event_value{};
  event_value.prev_hash = sha256(prev_value);
  event_value.proposal_header.block_height = block_height;
  event_value.proposal_header.events_hash = event_result.root_hash();
  auto event_commit_res =
      event_sched_client.eventsched_commit_block(block_number, event_value)();
  if (event_commit_res != RPCError::kRPCNone) {
    panic("Failed to commit event block");
  }
  prev_value = event_value;
  auto event_commit_time = x23::fastclock::now();
  // Stamped from the same clock the probe stops on, so the recorded span is
  // commit-return to query-visible with no second clock in it.
  if (indexer_probe != nullptr) {
    indexer_probe->record_commit(block_height, event_commit_time);
  }
  std::println("closing: issue_time {} close_time {} commit_time {} "
               "done_time {} event_commit_time {}",
               (issue_time - start_time).count(),
               (close_time - issue_time).count(),
               (commit_time - close_time).count(),
               (done_time - commit_time).count(),
               (event_commit_time - done_time).count());
}

// Starts the topology on the control servers at [server_addrs]: hog-event on
// the first, hog-db on the second, hog-indexer on the third and an executor on
// each of the rest. Returns once every executor reports ready. Destruction
// stops the servers.
class MultiexecCluster {
public:
  explicit MultiexecCluster(const std::vector<std::string> &server_addrs)
  {
    if (server_addrs.size() < 4) {
      // 1 event server, 1 db, 1 indexer, and the rest executors
      panic("At least 4 servers are required");
    }

    control_server_handles_.reserve(server_addrs.size());
    for (const auto &server_addr : server_addrs) {
      control_server_handles_.emplace_back(server_addr);
    }

    uint16_t sched_exec_port = sched_exec_test_.start_server(0);
    uint16_t sched_db_port = sched_db_test_.start_server(0);
    uint16_t sched_event_port = sched_event_test_.start_server(0);

    auto work_dir = control_server_handles_[0].ci_client.query_work_dir();
    if (work_dir.error_code() != CIServerErrorCode::kCIServerErrorCodeOK) {
      error("Failed to query work dir: {}", work_dir.error_code());
      panic("Failed to query work dir");
    }

    std::string work_dir_str(work_dir.work_dir().begin(),
                             work_dir.work_dir().end());
    info("using work dir: {}", work_dir_str);

    std::string hostname;
    CHECK_EQ(get_hostname(hostname), 0);
    std::string mempool_ip = hog_ip(hostname);

    for (auto &control_server_handle : control_server_handles_) {
      if (control_server_handle.ci_client.clear_work_dir() !=
          CIServerErrorCode::kCIServerErrorCodeOK) {
        panic("Failed to clear work dir");
      }
    }

    size_t server_idx = 0;
    control_server_handles_[server_idx].ci_client.start_server(
        "hogd/hog_event/hog-event",
        {"--bind-ip", hog_ip(server_addrs[server_idx]), "--mempool-addr",
         std::format("{}:{}", mempool_ip, sched_event_port), "--work-dir",
         work_dir_str, "--indexer-port",
         std::to_string(kBenchEventIndexerPort)});
    const std::string event_ip = hog_ip(server_addrs[server_idx]);
    server_idx++;

    EventCard event_card = sched_event_test_->impl()->await_event_card();
    sched_exec_test_->impl()->register_event_card(event_card);

    control_server_handles_[server_idx].ci_client.start_server(
        "hogd/hog_db/hog-db",
        {"--db-path", work_dir_str, "--ram-bytes", "10000000000", "--bind-ip",
         hog_ip(server_addrs[server_idx]), "--coordinator-addr",
         std::format("{}:{}", mempool_ip, sched_db_port)});
    server_idx++;

    // Started before the executors so it is already draining the outbox by
    // the time the first block commits, which is the steady state the latency
    // number is supposed to describe.
    indexer_ip_ = hog_ip(server_addrs[server_idx]);
    control_server_handles_[server_idx].ci_client.start_server(
        "hogd/hog_indexer/hog-indexer",
        {"--bind-ip", indexer_ip_, "--event-addr",
         std::format("{}:{}", event_ip, kBenchEventIndexerPort), "--db-path",
         work_dir_str, "--block-explorer-port",
         std::to_string(kBenchIndexerBlockExplorerPort)});
    server_idx++;

    for (; server_idx < server_addrs.size(); server_idx++) {
      control_server_handles_[server_idx].ci_client.start_server(
          "hogd/hog_exec/hog-exec",
          {"--bind-ip", hog_ip(server_addrs[server_idx]), "--coordinator-addr",
           std::format("{}:{}", mempool_ip, sched_exec_port)});
    }

    auto db_cards = sched_db_test_->impl()->await_shard_cards(1);
    CHECK(db_exec_addr_.parse(db_cards[0].address_for_exec));
    sched_exec_test_->impl()->register_shard_cards(db_cards);
    exec_cards_ =
        sched_exec_test_->impl()->await_executor_cards(server_addrs.size() - 3);

    event_sched_client_ = std::make_unique<bench_event_sched_client_t>(
        connect_to_event_sched_server(event_card.address_for_sched));
    assert(db_cards.size() == 1);
    db_sched_client_ = std::make_unique<bench_db_sched_client_t>(
        connect_to_db_server(db_cards[0]));

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
    for (auto &exec_card : exec_cards_) {
      await_ready(exec_card);
    }
  }

  MultiexecCluster(const MultiexecCluster &) = delete;
  MultiexecCluster &operator=(const MultiexecCluster &) = delete;
  MultiexecCluster(MultiexecCluster &&) = delete;
  MultiexecCluster &operator=(MultiexecCluster &&) = delete;
  ~MultiexecCluster() = default;

  void
  write_genesis_configuration(const GenesisConfiguration &genesis_configuration)
  {
    ConsensusValue genesis_value{};
    genesis_value.proposal_header.block_height = 0;
    genesis_value.proposal_header.config_hash =
        compute_config_hash(genesis_configuration);
    auto res = db_sched_client_->dbsched_load_genesis_configuration(
        genesis_configuration, genesis_value);
    if (res != DbSchedResult::DbSchedResultOK) {
      panic("Failed to write genesis block");
    }
  }

  // [n_connections_per_executor] connections to each executor, interleaved
  // so that consecutive connections go to different executors.
  std::vector<exec_client_t>
  connect_exec_clients(size_t n_connections_per_executor) const
  {
    const size_t n_connections =
        n_connections_per_executor * exec_cards_.size();
    std::vector<exec_client_t> exec_clients;
    exec_clients.reserve(n_connections);
    for (size_t i = 0; i < n_connections; i++) {
      exec_clients.push_back(
          connect_to_exec_server(exec_cards_[i % exec_cards_.size()]));
    }
    return exec_clients;
  }

  size_t num_executors() const { return exec_cards_.size(); }

  // Like connect_exec_clients(), but each connection is polled in the next
  // LLC lane of this process (LlcLanes), with the pool its coroutines should
  // run in.  One CoroPoll for every connection saturates its poll thread at
  // a few executors' worth of replies; lanes split that load and keep each
  // connection's work on one L3.  Without lanes, every connection gets
  // default_coropoll() and default_threadpool().
  struct LaneExecClients {
    std::vector<exec_client_t> clients;
    std::vector<x23::ThreadPool *> pools;
  };
  LaneExecClients
  connect_exec_clients_in_lanes(size_t n_connections_per_executor) const
  {
    const size_t n_connections =
        n_connections_per_executor * exec_cards_.size();
    LaneExecClients out;
    out.clients.reserve(n_connections);
    out.pools.reserve(n_connections);
    for (size_t i = 0; i < n_connections; i++) {
      LlcLane *lane = LlcLanes::get().next();
      x23::CoroPoll &cp = lane ? lane->cp : default_coropoll();
      out.clients.push_back(
          connect_to_exec_server(exec_cards_[i % exec_cards_.size()], cp));
      out.pools.push_back(&cp.pool_);
    }
    return out;
  }

  // Executor i's host, as connect_exec_clients() dials it.
  std::string exec_host(size_t i) const
  {
    x23::HostPort host_port;
    if (!host_port.parse(exec_cards_[i].address))
      return "?";
    return std::string(host_port.host.data(), host_port.host.size());
  }

  // Where executors reach hog-db, for a client that wants to read from it
  // the way they do.
  const x23::HostPort &db_exec_addr() const { return db_exec_addr_; }

  std::string indexer_block_explorer_addr() const
  {
    return std::format("{}:{}", indexer_ip_, kBenchIndexerBlockExplorerPort);
  }

  bench_db_sched_client_t &db_sched_client() { return *db_sched_client_; }
  bench_event_sched_client_t &event_sched_client()
  {
    return *event_sched_client_;
  }

private:
  // Declared first so it is destroyed last: stopping the servers comes after
  // everything that talks to them has gone away.
  std::vector<ControlServerHandle> control_server_handles_;

  RpcServer<SchedExecTest> sched_exec_test_;
  RpcServer<MockSchedDB> sched_db_test_;
  RpcServer<MockSchedEvent> sched_event_test_;

  x23::HostPort db_exec_addr_;
  std::string indexer_ip_;
  std::vector<ExecCard> exec_cards_;

  std::unique_ptr<bench_event_sched_client_t> event_sched_client_;
  std::unique_ptr<bench_db_sched_client_t> db_sched_client_;
};

} // namespace hogd
